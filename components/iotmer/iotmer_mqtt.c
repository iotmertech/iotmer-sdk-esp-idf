/*
 * iotmer_mqtt.c — Internal MQTT publish helper.
 *
 * Uses esp_mqtt_client_enqueue() so delivery runs in the mqtt client task.
 * Safe to call from any task, including the MQTT event handler.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "iotmer_internal.h"

#define TAG "iotmer_mqtt"

esp_err_t iotmer_mqtt_publish(iotmer_client_t *client,
                              const char *topic,
                              const char *payload,
                              int qos,
                              int retain)
{
    if (!client || !topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!client->mqtt) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_enqueue(
        (esp_mqtt_client_handle_t)client->mqtt,
        topic, payload,
        0 /* len */, qos, retain,
        true /* store: enqueue regardless of QoS */);

    if (msg_id == -2) {
        ESP_LOGE(TAG, "outbox full: %s", topic);
        return ESP_ERR_NO_MEM;
    }
    if (msg_id < 0) {
        ESP_LOGE(TAG, "enqueue failed: %s", topic);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "queued: %s msg_id=%d", topic, msg_id);
    return ESP_OK;
}
