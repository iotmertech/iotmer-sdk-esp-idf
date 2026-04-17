# 03_lwt_presence

ESP-IDF example that enables MQTT Last Will (LWT) / presence behavior and keeps the connection alive.

## Code (overview)

```c
#include "iotmer_client.h"

iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
cfg.presence_lwt_enable = true;

iotmer_client_t client;
(void)iotmer_init(&client, &cfg);
(void)iotmer_connect(&client);
```

See `main/main.c` for the full example.

## Build / flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

## Notes

- Configure Wi‑Fi / workspace values via `menuconfig` (see `sdkconfig.defaults` for Kconfig symbol names).
- Canonical source also lives in the main repository: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/03_lwt_presence`
