# 04_config

ESP-IDF example that subscribes to the IOTMER config MQTT topics and demonstrates the gzip+base64 config flow callbacks.

## Code (overview)

```c
#include "iotmer_client.h"
#include "iotmer_config.h"

static iotmer_client_t s_client;
static iotmer_config_ctx_t s_cfg;
static uint8_t s_cfg_buf[65536];

static void on_config_mqtt(const char *topic, const char *payload, int len, void *ctx)
{
    (void)iotmer_config_on_mqtt(&s_cfg, (iotmer_client_t *)ctx, topic, payload, len, NULL, NULL);
}

void app_main(void)
{
    iotmer_config_ctx_init(&s_cfg, s_cfg_buf, sizeof(s_cfg_buf));
    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    (void)iotmer_init(&s_client, &cfg);
    (void)iotmer_connect(&s_client);
    (void)iotmer_subscribe_config(&s_client, on_config_mqtt, &s_client);
}
```

Note: the real example wires a full event callback; see `main/main.c`.

## Build / flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

## Notes

- Configure Wi‑Fi / workspace values via `menuconfig` (see `sdkconfig.defaults` for Kconfig symbol names).
- Canonical source also lives in the main repository: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/04_config`
