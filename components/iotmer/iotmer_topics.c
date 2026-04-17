/*
 * iotmer_topics.c — MQTT topic builder (cloud / console ACL).
 *
 * Console ACL: workspace slug + device key (see https://docs.iotmer.com/):
 *
 *   {workspace_slug}/{device_key}/telemetry   publish
 *   {workspace_slug}/{device_key}/state       publish
 *   {workspace_slug}/{device_key}/presence/   publish (retained, ONLINE/OFFLINE)
 *   {workspace_slug}/{device_key}/cmd/#       subscribe
 *   {workspace_slug}/{device_key}/config/#    subscribe
 *
 * iotmer_topics_build_publish() and subscribe helpers are free of dynamic allocation:
 * the output buffer is supplied by the caller.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "iotmer_internal.h"

#define TAG "iotmer_topics"

static bool slug_key_ok(const char *workspace_slug, const char *device_key)
{
    return workspace_slug && workspace_slug[0] != '\0' && device_key && device_key[0] != '\0';
}

esp_err_t iotmer_topics_build_publish(char *out, size_t out_len,
                                      const char *workspace_slug,
                                      const char *device_key,
                                      const char *suffix)
{
    if (!out || out_len == 0 || !slug_key_ok(workspace_slug, device_key) || !suffix) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(out, out_len, "%s/%s/%s", workspace_slug, device_key, suffix);
    if (n < 0 || (size_t)n >= out_len) {
        ESP_LOGE(TAG, "publish topic too long (need %d, have %zu)", n + 1, out_len);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t iotmer_topics_subscribe_cmd(char *out, size_t out_len,
                                      const char *workspace_slug,
                                      const char *device_key)
{
    if (!out || out_len == 0 || !slug_key_ok(workspace_slug, device_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(out, out_len, "%s/%s/cmd/#", workspace_slug, device_key);
    if (n < 0 || (size_t)n >= out_len) {
        ESP_LOGE(TAG, "cmd subscribe filter too long (need %d, have %zu)", n + 1, out_len);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t iotmer_topics_subscribe_config(char *out, size_t out_len,
                                         const char *workspace_slug,
                                         const char *device_key)
{
    if (!out || out_len == 0 || !slug_key_ok(workspace_slug, device_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(out, out_len, "%s/%s/config/#", workspace_slug, device_key);
    if (n < 0 || (size_t)n >= out_len) {
        ESP_LOGE(TAG, "config subscribe filter too long (need %d, have %zu)", n + 1, out_len);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool iotmer_topics_match_cmd(const char *topic, const char *workspace_slug,
                             const char *device_key)
{
    if (!topic || !slug_key_ok(workspace_slug, device_key)) {
        return false;
    }
    char p[256];
    int n = snprintf(p, sizeof(p), "%s/%s/cmd/", workspace_slug, device_key);
    if (n < 0 || (size_t)n >= sizeof(p)) {
        return false;
    }
    return strncmp(topic, p, (size_t)n) == 0;
}

bool iotmer_topics_match_config(const char *topic, const char *workspace_slug,
                                const char *device_key)
{
    if (!topic || !slug_key_ok(workspace_slug, device_key)) {
        return false;
    }
    char p[256];
    int n = snprintf(p, sizeof(p), "%s/%s/config/", workspace_slug, device_key);
    if (n < 0 || (size_t)n >= sizeof(p)) {
        return false;
    }
    return strncmp(topic, p, (size_t)n) == 0;
}
