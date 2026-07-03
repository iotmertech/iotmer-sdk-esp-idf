#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iotmer_client.h"

static const char *TAG = "02_telemetry";

static void on_command(const char *topic, const char *payload, int len, void *ctx)
{
    (void)ctx;
    ESP_LOGI("cmd", "Topic: %s", topic);
    ESP_LOGI("cmd", "Payload: %.*s", len, payload);
}

/* --- Optional MQTT lifecycle callbacks (iotmer_config_t) --- */

static void on_connected(iotmer_client_t *c, void *user_ctx)
{
    (void)c;
    (void)user_ctx;
    ESP_LOGI(TAG, "MQTT connected — (re)subscriptions re-issued by the SDK");
}

static void on_disconnected(iotmer_client_t *c, void *user_ctx)
{
    (void)c;
    (void)user_ctx;
    ESP_LOGW(TAG, "MQTT disconnected");
}

static void on_published(iotmer_client_t *c, int msg_id, void *user_ctx)
{
    (void)c;
    (void)user_ctx;
    ESP_LOGD(TAG, "PUBACK msg_id=%d", msg_id);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    /* Detect half-open connections: hard-reconnect after 45 s with no broker activity. */
    cfg.phantom_timeout_ms = 45000;
    cfg.on_connected    = on_connected;
    cfg.on_disconnected = on_disconnected;
    cfg.on_published    = on_published;

    iotmer_client_t client;

    err = iotmer_init(&client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Register the subscription up-front; the SDK (re)issues it on every connect. */
    err = iotmer_subscribe_commands(&client, on_command, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "subscribe failed: %s", esp_err_to_name(err));
        return;
    }

    err = iotmer_connect(&client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_connect failed: %s", esp_err_to_name(err));
        return;
    }

    while (!client.connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (1) {
        (void)iotmer_telemetry_publish(&client, "{\"temp\": 22.5, \"hum\": 60}");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
