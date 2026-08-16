#pragma once

/* Register this node with the mesh's SRP server so it's discoverable
   by name (`_carlab._udp`). Reads the per-device `hw` string from NVS
   and exposes an `ot carlab hw [value]` CLI to read/set it.

   Call once, after esp_openthread_start() and thread_autostart(). All
   registration is deferred until the node is attached to the mesh; a
   Thread state-change callback re-registers on reconnect. */
void carlab_srp_init(void);
