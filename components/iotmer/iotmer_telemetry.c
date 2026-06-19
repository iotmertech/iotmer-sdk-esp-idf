/*
 * iotmer_telemetry.c — Telemetry and state publish helpers.
 *
 * Both iotmer_telemetry_publish and iotmer_state_publish delegate to the
 * same internal publish_suffix function, differing only in the topic suffix.
 *
 * QoS 1 is used for all publishes: at-least-once delivery with broker ACK.
 * Publishes are enqueued via iotmer_mqtt_publish() for mqtt-task delivery.
 */

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "iotmer_internal.h"

#define TAG "iotmer_telemetry"

static esp_err_t publish_suffix(iotmer_client_t *client,
                                const char *suffix,
                                const char *json)
{
    if (!client || !suffix || !json) return ESP_ERR_INVALID_ARG;

    if (!client->mqtt || !client->connected) {
        ESP_LOGW(TAG, "publish(%s) skipped — not connected", suffix);
        return ESP_ERR_INVALID_STATE;
    }

    if (client->creds.workspace_slug[0] == '\0' || client->creds.device_key[0] == '\0') {
        ESP_LOGE(TAG, "publish(%s) failed — workspace_slug or device_key empty", suffix);
        return ESP_ERR_INVALID_STATE;
    }

    char topic[256];
    esp_err_t err = iotmer_topics_build_publish(topic, sizeof(topic),
                                                client->creds.workspace_slug,
                                                client->creds.device_key,
                                                suffix);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "topic build failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t pub_err = iotmer_mqtt_publish(client, topic, json, 1 /* QoS 1 */, 0 /* retain */);
    if (pub_err != ESP_OK) {
        ESP_LOGE(TAG, "publish(%s) failed: %s", suffix, esp_err_to_name(pub_err));
        return pub_err;
    }

    ESP_LOGD(TAG, "publish queued: topic=%s", topic);
    return ESP_OK;
}

esp_err_t iotmer_telemetry_publish(iotmer_client_t *client, const char *json)
{
    return publish_suffix(client, "telemetry", json);
}

esp_err_t iotmer_state_publish(iotmer_client_t *client, const char *json)
{
    return publish_suffix(client, "state", json);
}
