# 01_provisioning

Minimal ESP-IDF example that initializes NVS and calls `iotmer_init()` to print basic device credentials after provisioning-related setup.

## Code (overview)

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "iotmer_client.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return;
    }

    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    iotmer_client_t client;

    err = iotmer_init(&client, &cfg);
    if (err != ESP_OK) {
        return;
    }

    ESP_LOGI("main", "device_id: %s", client.creds.device_id);
}
```

The full file is `main/main.c` in this example directory.

## Build / flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

## Notes

- Configure Wi‑Fi / workspace values via `menuconfig` (see `sdkconfig.defaults` for Kconfig symbol names).
- Canonical source also lives in the main repository: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/01_provisioning`
