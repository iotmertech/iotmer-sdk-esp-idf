#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hw_config.h"
#include "iotmer_app.h"
#include "iotmer_client.h"

static const char *TAG = "cloud";

static iotmer_client_t s_client;

static void on_command(const char *topic, const char *payload, int len, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "command topic=%s payload=%.*s", topic, len, payload);
    /* Dispatch to your actuators / Modbus writes in components/hw/ */
}

static void on_connected(iotmer_client_t *c, void *user_ctx)
{
    (void)c;
    (void)user_ctx;
    ESP_LOGI(TAG, "MQTT connected");
}

static void on_disconnected(iotmer_client_t *c, void *user_ctx)
{
    (void)c;
    (void)user_ctx;
    ESP_LOGW(TAG, "MQTT disconnected");
}

static void telemetry_task(void *arg)
{
    iotmer_client_t *client = (iotmer_client_t *)arg;
    char extra[128];
    char payload[256];

    while (1) {
        extra[0] = '\0';
        if (hw_read_telemetry(extra, sizeof(extra)) == ESP_OK && extra[0] != '\0') {
            snprintf(payload, sizeof(payload), "{\"ts\":0,%s}", extra);
        } else {
            snprintf(payload, sizeof(payload), "{\"ts\":0,\"status\":\"ok\"}");
        }

        esp_err_t err = iotmer_telemetry_publish(client, payload);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "telemetry publish: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t iotmer_app_start(void)
{
    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    cfg.phantom_timeout_ms = 45000;
    cfg.on_connected = on_connected;
    cfg.on_disconnected = on_disconnected;

    esp_err_t err = iotmer_init(&s_client, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = iotmer_subscribe_commands(&s_client, on_command, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = iotmer_connect(&s_client);
    if (err != ESP_OK) {
        return err;
    }

    while (!s_client.connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    BaseType_t ok = xTaskCreate(telemetry_task, "telemetry", 4096, &s_client, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IOTMER cloud loop running (profile {{PROFILE}})");
    return ESP_OK;
}
