/*
 * iotmer_presence.c — Presence publish helper (ONLINE/OFFLINE).
 *
 * Presence is published as a retained plain-text payload to:
 *   {workspace_slug}/{device_key}/presence/
 *
 * Retained messages allow consumers to immediately see the latest device status.
 * OFFLINE is typically delivered by broker via MQTT LWT (last will).
 */

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "iotmer_internal.h"

#define TAG "iotmer_presence"

esp_err_t iotmer_presence_publish(iotmer_client_t *client, const char *status)
{
    if (!client || !status) return ESP_ERR_INVALID_ARG;

    if (!client->mqtt || !client->connected) {
        ESP_LOGW(TAG, "publish presence skipped — not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (client->creds.workspace_slug[0] == '\0' || client->creds.device_key[0] == '\0') {
        ESP_LOGE(TAG, "publish presence failed — workspace_slug or device_key empty");
        return ESP_ERR_INVALID_STATE;
    }

    char topic[256];
    esp_err_t err = iotmer_topics_build_publish(topic, sizeof(topic),
                                                client->creds.workspace_slug,
                                                client->creds.device_key,
                                                "presence/");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "topic build failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * len=0 tells the MQTT client to use strlen(status).
     * retain=1 so subscribers get the latest status immediately.
     * QoS=1 for at-least-once delivery.
     */
    int msg_id = esp_mqtt_client_publish(
        (esp_mqtt_client_handle_t)client->mqtt,
        topic, status,
        0 /* len */, 1 /* QoS 1 */, 1 /* retain */);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish(%s) enqueue failed", topic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "presence published: %s => %s (msg_id=%d)", topic, status, msg_id);
    return ESP_OK;
}

