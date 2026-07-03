/*
 * iotmer_presence.c — Presence publish helper (online/offline).
 *
 * Presence is published as a retained JSON payload to:
 *   {workspace_slug}/{device_key}/presence
 *
 * Payload: {"status":"online","ts":<unix seconds>} — ts is 0 when the device
 * clock is not synced yet. The MQTT LWT (broker-delivered on unexpected
 * disconnect) uses the same shape: {"status":"offline","ts":0}.
 *
 * Retained messages allow consumers to immediately see the latest device status.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"

#include "iotmer_internal.h"

#define TAG "iotmer_presence"

/* Clock sanity floor: anything below this means SNTP hasn't synced (ts -> 0). */
#define PRESENCE_TS_MIN_VALID 1000000000LL /* 2001-09-09 */

esp_err_t iotmer_presence_build_payload(char *out, size_t out_len, const char *status)
{
    if (!out || out_len == 0 || !status || status[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Normalise to lowercase so "ONLINE" and "online" produce the same JSON. */
    char norm[16];
    size_t i = 0;
    for (; status[i] != '\0' && i < sizeof(norm) - 1; ++i) {
        norm[i] = (char)tolower((unsigned char)status[i]);
    }
    norm[i] = '\0';

    long long ts = (long long)time(NULL);
    if (ts < PRESENCE_TS_MIN_VALID) {
        ts = 0;
    }

    int n = snprintf(out, out_len, "{\"status\":\"%s\",\"ts\":%lld}", norm, ts);
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
                                                "presence");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "topic build failed: %s", esp_err_to_name(err));
        return err;
    }

    char payload[64];
    err = iotmer_presence_build_payload(payload, sizeof(payload), status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "payload build failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t pub_err = iotmer_mqtt_publish(client, topic, payload, 1 /* QoS 1 */, 1 /* retain */);
    if (pub_err != ESP_OK) {
        ESP_LOGE(TAG, "publish presence failed: %s", esp_err_to_name(pub_err));
        return pub_err;
    }

    ESP_LOGI(TAG, "presence queued: %s => %s", topic, payload);
    return ESP_OK;
}
