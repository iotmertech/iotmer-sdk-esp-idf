#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iotmer_client.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("main", "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    cfg.presence_lwt_enable = true;

    iotmer_client_t client;

    err = iotmer_init(&client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE("main", "iotmer_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = iotmer_connect(&client);
    if (err != ESP_OK) {
        ESP_LOGE("main", "iotmer_connect failed: %s", esp_err_to_name(err));
        return;
    }

    /*
     * Presence flow:
     * - MQTT_EVENT_CONNECTED publishes retained "ONLINE" to:
     *     {workspace_slug}/{device_key}/presence/
     * - If power/network drops without a clean MQTT disconnect,
     *   broker publishes retained LWT "OFFLINE" to the same topic.
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
