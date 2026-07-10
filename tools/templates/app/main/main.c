/**
 * Boot order for custom hardware + IOTMER:
 *   1. NVS
 *   2. hw_init()  — your pins, buses, sensors
 *   3. cloud      — Wi-Fi, provision, MQTT (components/cloud)
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "hw_config.h"
#include "iotmer_app.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = hw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hw_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = iotmer_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_app_start failed: %s", esp_err_to_name(err));
        return;
    }
}
