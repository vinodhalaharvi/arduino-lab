/*
 * thread_autostart.c — bring up Thread at boot if we already have creds.
 *
 * The out-of-box ot_cli sits at the CLI after boot and waits for
 * `ifconfig up` / `thread start`. That is fine on a bench with a USB
 * host attached, useless on a USB charger with no one to type. This
 * closes that gap without changing the CLI behaviour for the empty-NVS
 * case, so first-time provisioning still works the normal way.
 */

#include "thread_autostart.h"

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/thread.h"

static const char *TAG = "autostart";

void thread_autostart(void)
{
    esp_openthread_lock_acquire(portMAX_DELAY);

    otInstance *inst = esp_openthread_get_instance();

    if (!otDatasetIsCommissioned(inst)) {
        ESP_LOGI(TAG, "no dataset, waiting for provisioning");
        esp_openthread_lock_release();
        return;
    }

    /* Force rdn: receiver on when idle, full Thread device, full network
       data. FTD builds default to this, but a stray `ot mode` from an
       earlier session persists in NVS, and coming up as an MTD with a
       236 s poll period queues inbound CoAP for minutes. */
    otLinkModeConfig mode = {
        .mRxOnWhenIdle = true,
        .mDeviceType   = true,
        .mNetworkData  = true,
    };
    if (otThreadSetLinkMode(inst, mode) != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "failed to set link mode rdn");
    }

    if (otIp6SetEnabled(inst, true) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otIp6SetEnabled failed");
        esp_openthread_lock_release();
        return;
    }

    if (otThreadSetEnabled(inst, true) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otThreadSetEnabled failed");
        esp_openthread_lock_release();
        return;
    }

    ESP_LOGI(TAG, "dataset found, starting thread");
    esp_openthread_lock_release();
}
