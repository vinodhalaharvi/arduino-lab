/*
 * carlab_srp.c — SRP client registration for the carlab mesh.
 *
 * On attach, register a host (node-<last4>) and one service instance
 * of type "_carlab._udp" on port 5683 with the border router's SRP
 * server. The BR re-publishes over mDNS to the LAN, so from a Mac:
 *
 *   dns-sd -B _carlab._udp
 *   dns-sd -L node-b8fc _carlab._udp
 *
 * The TXT record carries what the controller needs to plan a UI:
 *
 *   type = actuator     — role, fixed at build time
 *   res  = led,bri,fx,speed,relay
 *                       — resources this firmware serves, fixed
 *   hw   = led,relay    — what is physically wired, from NVS
 *   ver  = 1            — TXT schema version, fixed
 *
 * The res/hw split matters: two nodes running identical firmware may
 * ship different hardware. Both expose /relay, but only one has the
 * relay module bolted on GPIO2 — the controller offers a relay button
 * only where `hw` mentions it.
 *
 * `hw` lives in NVS so a single firmware image ships to every node
 * and per-device config is set once at provisioning time via
 * `ot carlab hw led,relay` on the console.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "openthread/cli.h"
#include "openthread/dns.h"
#include "openthread/instance.h"
#include "openthread/link.h"
#include "openthread/srp_client.h"
#include "openthread/thread.h"

#include "carlab_srp.h"

static const char *TAG = "carlab_srp";

#define NVS_NS       "carlab"
#define NVS_KEY_HW   "hw"

#define SERVICE_TYPE "_carlab._udp"
#define SERVICE_PORT 5683                 /* OT_DEFAULT_COAP_PORT */
#define TXT_VER      "1"
#define TXT_TYPE     "actuator"
#define TXT_RES      "led,bri,fx,speed,relay"

/* -------------------------------------------------------------- state --- */
/*
 * The strings/buffers passed to otSrpClient* MUST persist and stay
 * constant while the service is registered. Statics are the simplest
 * way to guarantee that; the pointers we hand to OT below all point
 * inside this file.
 */

static char s_hostname[24];   /* "node-b8fc" — also used as instance name */
static char s_hw[64] = "led"; /* default; overwritten from NVS if present */

static otDnsTxtEntry s_txt[4];
static otSrpClientService s_service;

static bool s_initialised;    /* did we set hostname / add service yet? */

/* ----------------------------------------------------------- helpers --- */

static void load_hw_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no carlab nvs namespace yet, hw=%s", s_hw);
        return;
    }
    size_t len = sizeof(s_hw);
    esp_err_t err = nvs_get_str(h, NVS_KEY_HW, s_hw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "hw not set in nvs, defaulting to %s", s_hw);
    } else {
        ESP_LOGI(TAG, "hw loaded from nvs: %s", s_hw);
    }
}

static esp_err_t save_hw_to_nvs(const char *v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_HW, v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void build_hostname(void)
{
    const otExtAddress *ext =
        otLinkGetExtendedAddress(esp_openthread_get_instance());
    snprintf(s_hostname, sizeof(s_hostname), "node-%02x%02x",
             ext->m8[6], ext->m8[7]);
}

/* (Re)populate the TXT entry array. mKey is a constant literal; mValue
   points into a static string. Called before otSrpClientAddService and
   again after `hw` changes. */
static void build_txt_entries(void)
{
    s_txt[0].mKey         = "type";
    s_txt[0].mValue       = (const uint8_t *)TXT_TYPE;
    s_txt[0].mValueLength = strlen(TXT_TYPE);

    s_txt[1].mKey         = "res";
    s_txt[1].mValue       = (const uint8_t *)TXT_RES;
    s_txt[1].mValueLength = strlen(TXT_RES);

    s_txt[2].mKey         = "hw";
    s_txt[2].mValue       = (const uint8_t *)s_hw;
    s_txt[2].mValueLength = strlen(s_hw);

    s_txt[3].mKey         = "ver";
    s_txt[3].mValue       = (const uint8_t *)TXT_VER;
    s_txt[3].mValueLength = strlen(TXT_VER);
}

static void build_service_struct(void)
{
    memset(&s_service, 0, sizeof(s_service));
    s_service.mName          = SERVICE_TYPE;
    s_service.mInstanceName  = s_hostname;
    s_service.mPort          = SERVICE_PORT;
    s_service.mTxtEntries    = s_txt;
    s_service.mNumTxtEntries = sizeof(s_txt) / sizeof(s_txt[0]);
}

/* ---------------------------------------------------------- callbacks -- */

static const char *item_state_str(otSrpClientItemState s)
{
    switch (s) {
    case OT_SRP_CLIENT_ITEM_STATE_TO_ADD:     return "to-add";
    case OT_SRP_CLIENT_ITEM_STATE_ADDING:     return "adding";
    case OT_SRP_CLIENT_ITEM_STATE_TO_REFRESH: return "to-refresh";
    case OT_SRP_CLIENT_ITEM_STATE_REFRESHING: return "refreshing";
    case OT_SRP_CLIENT_ITEM_STATE_TO_REMOVE:  return "to-remove";
    case OT_SRP_CLIENT_ITEM_STATE_REMOVING:   return "removing";
    case OT_SRP_CLIENT_ITEM_STATE_REGISTERED: return "registered";
    case OT_SRP_CLIENT_ITEM_STATE_REMOVED:    return "removed";
    default:                                  return "?";
    }
}

static void srp_client_cb(otError err,
                          const otSrpClientHostInfo *host,
                          const otSrpClientService  *services,
                          const otSrpClientService  *removed,
                          void *ctx)
{
    (void)ctx; (void)removed;
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "srp update: error %d (host %s)", err,
                 host ? item_state_str(host->mState) : "?");
        return;
    }
    if (host != NULL) {
        ESP_LOGI(TAG, "srp host %s: %s",
                 host->mName ? host->mName : "?",
                 item_state_str(host->mState));
    }
    for (const otSrpClientService *s = services; s != NULL; s = s->mNext) {
        ESP_LOGI(TAG, "srp service %s.%s: %s",
                 s->mInstanceName ? s->mInstanceName : "?",
                 s->mName         ? s->mName         : "?",
                 item_state_str(s->mState));
    }
}

static void srp_autostart_cb(const otSockAddr *server, void *ctx)
{
    (void)ctx;
    if (server == NULL) {
        ESP_LOGI(TAG, "srp autostart: no server available");
    } else {
        char addr[OT_IP6_ADDRESS_STRING_SIZE];
        otIp6AddressToString(&server->mAddress, addr, sizeof(addr));
        ESP_LOGI(TAG, "srp autostart: server %s port %u", addr, server->mPort);
    }
}

/* One-shot setup: called from the state-change callback the first time
   we see an attached role. Assumes the OT lock is held (which it is,
   because state-changed runs on the OT task). */
static void do_register(otInstance *inst)
{
    build_hostname();
    build_txt_entries();
    build_service_struct();

    otSrpClientSetCallback(inst, srp_client_cb, NULL);

    otError err = otSrpClientSetHostName(inst, s_hostname);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otSrpClientSetHostName(%s) failed: %d",
                 s_hostname, err);
        return;
    }

    err = otSrpClientEnableAutoHostAddress(inst);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otSrpClientEnableAutoHostAddress failed: %d", err);
        return;
    }

    err = otSrpClientAddService(inst, &s_service);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otSrpClientAddService failed: %d", err);
        return;
    }

    /* Auto-start mode monitors Network Data for SRP servers and starts
       the client when one appears. Default mode is already enabled in
       the ESP32-C6 FTD config, but calling explicitly is cheap and
       self-documenting. */
    otSrpClientEnableAutoStartMode(inst, srp_autostart_cb, NULL);

    s_initialised = true;
    ESP_LOGI(TAG, "registered as %s._carlab._udp port %d hw=%s",
             s_hostname, SERVICE_PORT, s_hw);
}

static void state_changed_cb(otChangedFlags flags, void *ctx)
{
    (void)ctx;
    if ((flags & OT_CHANGED_THREAD_ROLE) == 0) {
        return;
    }
    otInstance *inst = esp_openthread_get_instance();
    otDeviceRole role = otThreadGetDeviceRole(inst);

    /* Attached == CHILD, ROUTER, or LEADER. Skip while disabled or
       detached — SRP has nowhere to talk to. */
    if (role != OT_DEVICE_ROLE_CHILD && role != OT_DEVICE_ROLE_ROUTER &&
        role != OT_DEVICE_ROLE_LEADER) {
        return;
    }

    if (s_initialised) {
        /* Auto-start + auto-host-address handle reconnects transparently;
           nothing to do here on subsequent attaches. */
        return;
    }
    ESP_LOGI(TAG, "attached (role %d), setting up srp", role);
    do_register(inst);
}

/* Called from the CLI task on `ot carlab hw NEW`. Rebuilds the TXT
   entries and re-adds the service so the SRP client re-registers with
   the updated TXT next time it talks to the server. */
static void update_hw_live(void)
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *inst = esp_openthread_get_instance();

    if (s_initialised) {
        /* Clear locally then re-add: the SRP client uses the new TXT
           when it composes the next update. The server sees an update
           to the existing (host, instance) pair — not a remove/re-add. */
        otSrpClientClearService(inst, &s_service);
        build_txt_entries();
        build_service_struct();
        otError err = otSrpClientAddService(inst, &s_service);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "re-add after hw change failed: %d", err);
        } else {
            ESP_LOGI(TAG, "hw updated, service re-added: %s", s_hw);
        }
    } else {
        /* Not attached yet — nothing to update, the fresh s_hw will
           be picked up when do_register() runs on first attach. */
        build_txt_entries();
        ESP_LOGI(TAG, "hw updated (not attached yet): %s", s_hw);
    }
    esp_openthread_lock_release();
}

/* ---------------------------------------------------------------- CLI -- */

static otError cmd_carlab(void *ctx, uint8_t argc, char *argv[])
{
    (void)ctx;
    if (argc == 0 || strcmp(argv[0], "hw") != 0) {
        otCliOutputFormat("usage: carlab hw [value]\r\n");
        return OT_ERROR_INVALID_ARGS;
    }
    if (argc == 1) {
        otCliOutputFormat("%s\r\n", s_hw);
        return OT_ERROR_NONE;
    }
    if (strlen(argv[1]) >= sizeof(s_hw)) {
        otCliOutputFormat("too long (max %u)\r\n",
                          (unsigned)(sizeof(s_hw) - 1));
        return OT_ERROR_INVALID_ARGS;
    }

    /* Persist first, then live-update. If NVS fails we don't want the
       running SRP state to diverge from what's on disk. */
    esp_err_t nerr = save_hw_to_nvs(argv[1]);
    if (nerr != ESP_OK) {
        otCliOutputFormat("nvs write failed: %d\r\n", nerr);
        return OT_ERROR_FAILED;
    }
    strncpy(s_hw, argv[1], sizeof(s_hw) - 1);
    s_hw[sizeof(s_hw) - 1] = '\0';

    update_hw_live();
    return OT_ERROR_NONE;
}

static const otCliCommand s_cli_cmds[] = {
    { "carlab", cmd_carlab },
};

/* ---------------------------------------------------------------- init - */

void carlab_srp_init(void)
{
    load_hw_from_nvs();

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *inst = esp_openthread_get_instance();

    otError err = otCliSetUserCommands(s_cli_cmds,
                                       sizeof(s_cli_cmds) / sizeof(s_cli_cmds[0]),
                                       NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "otCliSetUserCommands failed: %d", err);
    }

    err = otSetStateChangedCallback(inst, state_changed_cb, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otSetStateChangedCallback failed: %d", err);
    }

    /* If we're already attached at init time (e.g. warm-start with a
       cached parent), fire the setup path once now — the callback only
       runs on transitions, not on the current state. */
    otDeviceRole role = otThreadGetDeviceRole(inst);
    if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
        role == OT_DEVICE_ROLE_LEADER) {
        ESP_LOGI(TAG, "already attached at init, registering now");
        do_register(inst);
    } else {
        ESP_LOGI(TAG, "waiting for attach before srp registration");
    }
    esp_openthread_lock_release();
}
