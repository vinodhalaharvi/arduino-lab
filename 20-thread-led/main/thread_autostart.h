#pragma once

/* If NVS holds a commissioned Thread dataset, force link mode to rdn
   (receiver-on-when-idle, full device, full network data), then bring
   up IPv6 and start Thread. If no dataset is present, log and return —
   the CLI is left available for provisioning.

   Call after esp_openthread_start(), which creates the instance. */
void thread_autostart(void);
