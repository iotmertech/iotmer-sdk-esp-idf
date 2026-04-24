#include "esp_log.h"
#include "nvs_flash.h"

#include "iotmer_client.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE("main", "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    iotmer_client_t client;

    err = iotmer_init(&client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE("main", "iotmer_init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI("main", "device_id: %s", client.creds.device_id);
    ESP_LOGI("main", "mqtt_host: %s", client.creds.mqtt_host);
}

