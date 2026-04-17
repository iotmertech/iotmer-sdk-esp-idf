# 02_telemetry

ESP-IDF example that connects over MQTT and periodically publishes telemetry JSON.

## Code (overview)

```c
#include "iotmer_client.h"

iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
iotmer_client_t client;

static void on_command(const char *topic, const char *payload, int len, void *ctx)
{
    (void)topic;
    (void)payload;
    (void)len;
    (void)ctx;
}

/* after iotmer_init(), iotmer_connect(), and client.connected == true */
(void)iotmer_subscribe_commands(&client, on_command, NULL);
(void)iotmer_telemetry_publish(&client, "{\"temp\": 22.5, \"hum\": 60}");
```

The full flow (including the connect/wait loop) is in `main/main.c`.

## Build / flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

## Notes

- Configure Wi‑Fi / workspace values via `menuconfig` (see `sdkconfig.defaults` for Kconfig symbol names).
- Canonical source also lives in the main repository: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/02_telemetry`
