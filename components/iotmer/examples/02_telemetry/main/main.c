#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iotmer_client.h"

static void on_command(const char *topic, const char *payload, int len, void *ctx)
{
    (void)ctx;
    ESP_LOGI("cmd", "Topic: %s", topic);
    ESP_LOGI("cmd", "Payload: %.*s", len, payload);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
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

    err = iotmer_connect(&client);
    if (err != ESP_OK) {
        ESP_LOGE("main", "iotmer_connect failed: %s", esp_err_to_name(err));
        return;
    }

    while (!client.connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    err = iotmer_subscribe_commands(&client, on_command, NULL);
    if (err != ESP_OK) {
        ESP_LOGE("main", "subscribe failed: %s", esp_err_to_name(err));
        return;
    }

    while (1) {
        (void)iotmer_telemetry_publish(&client, "{\"temp\": 22.5, \"hum\": 60}");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
