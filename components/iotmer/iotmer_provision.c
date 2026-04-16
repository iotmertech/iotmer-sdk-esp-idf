#include <stdio.h>
#include <string.h>

#include <limits.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cJSON.h"
#include "sdkconfig.h"

#include "iotmer_internal.h"

#define TAG "iotmer_provision"

/* Large enough for provision JSON + future fields; 4096 was easy to exceed → truncated parse. */
#define IOTMER_PROVISION_JSON_MAX 8192

/*
 * Do not use esp_http_client_perform() here: in ESP-IDF it reads and discards the
 * response body internally, so a follow-up esp_http_client_read() sees an empty body
 * and JSON parsing fails. Use open -> write -> fetch_headers -> read instead.
 */
static esp_err_t http_post_json(const char *url, const char *auth_code,
                               const char *body,
                               char *resp, size_t resp_len,
                               int *http_status_out)
{
    if (!url || !auth_code || !body || !resp || resp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t body_len = strlen(body);
    if (body_len > (size_t)INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_IOTMER_PROVISION_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        /* ESP-IDF v6+: TLS requires an explicit server verification option for HTTPS. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(c, "Content-Type", "application/json");
    if (err != ESP_OK) goto out;
    err = esp_http_client_set_header(c, "iotmer-auth-code", auth_code);
    if (err != ESP_OK) goto out;

    ESP_LOGI(TAG, "HTTPS POST %s", url);

    err = esp_http_client_open(c, (int)body_len);
    if (err != ESP_OK) {
        goto out;
    }

    int written = 0;
    while (written < (int)body_len) {
        int w = esp_http_client_write(c, body + written, (int)body_len - written);
        if (w <= 0) {
            err = ESP_FAIL;
            goto out;
        }
        written += w;
    }

    int64_t hdr = esp_http_client_fetch_headers(c);
    if (hdr < 0) {
        err = ESP_ERR_HTTP_FETCH_HEADER;
        goto out;
    }

    int status = esp_http_client_get_status_code(c);
    if (http_status_out) {
        *http_status_out = status;
    }

    int total = 0;
    while (1) {
        int r = esp_http_client_read(c, resp + total, (int)(resp_len - 1 - (size_t)total));
        if (r < 0) {
            err = ESP_FAIL;
            goto out;
        }
        if (r == 0) {
            break;
        }
        total += r;
        if ((size_t)total >= resp_len - 1) {
            break;
        }
    }
    resp[total] = '\0';

    int content_len = esp_http_client_get_content_length(c);
    if (content_len > 0 && total < content_len) {
        ESP_LOGW(TAG, "provision response truncated: read %d bytes, Content-Length=%d (raise "
                      "IOTMER_PROVISION_JSON_MAX / buffer in http_post_json caller)",
                 total, content_len);
    } else if ((size_t)total >= resp_len - 1) {
        ESP_LOGW(TAG, "provision response filled buffer (%zu bytes) — body may be truncated",
                 resp_len);
    }

    err = ESP_OK;

out:
    esp_http_client_cleanup(c);
    return err;
}

static esp_err_t json_copy_string(cJSON *obj, const char *key, char *out, size_t out_len)
{
    if (!obj || !key || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = strnlen(v->valuestring, out_len);
    if (n >= out_len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, v->valuestring, n);
    out[n] = '\0';
    return ESP_OK;
}

static esp_err_t json_copy_bool(cJSON *obj, const char *key, bool *out)
{
    if (!obj || !key || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t json_copy_int(cJSON *obj, const char *key, int *out)
{
    if (!obj || !key || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        *out = v->valueint;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t json_copy_u32(cJSON *obj, const char *key, uint32_t *out)
{
    if (!obj || !key || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble < 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *out = (uint32_t)v->valuedouble;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* Some backends wrap the payload in { "data": { ... } }. */
static cJSON *provision_inner_object(cJSON *root)
{
    cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(d) ? d : NULL;
}

/*
 * True when NVS (or caller) already holds everything needed to run MQTT without
 * calling the provision API again. Used for "telemetry-only" firmware that omits
 * CONFIG_IOTMER_PROVISION_AUTH_CODE after factory provisioning.
 */
static bool stored_session_complete(const iotmer_creds_t *c)
{
    if (!c) {
        return false;
    }
    if (c->device_id[0] == '\0' || c->device_key[0] == '\0') {
        return false;
    }
    if (c->workspace_slug[0] == '\0') {
        return false;
    }
    if (c->mqtt_host[0] == '\0' || c->mqtt_username[0] == '\0' || c->mqtt_password[0] == '\0') {
        return false;
    }
    return true;
}

esp_err_t iotmer_provision(iotmer_creds_t *out, bool *https_performed)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (https_performed) {
        *https_performed = false;
    }

    /*
     * Snapshot NVS-backed state so we can clear `out` for a clean JSON parse and still
     * send device_id on repeat calls. On any failure after clearing, restore the snapshot.
     */
    const iotmer_creds_t backup = *out;

    if (strlen(CONFIG_IOTMER_PROVISION_AUTH_CODE) == 0) {
        if (stored_session_complete(&backup)) {
            ESP_LOGI(TAG, "provision skipped (no auth code; using stored session)");
            return ESP_OK;
        }
        ESP_LOGE(TAG,
                 "provision auth empty and NVS session incomplete — "
                 "flash factory/provision firmware once or set IOTMER_PROVISION_AUTH_CODE");
        return ESP_ERR_INVALID_STATE;
    }

    if (strlen(CONFIG_IOTMER_WORKSPACE_ID) == 0) {
        ESP_LOGE(TAG, "workspace_id required for HTTPS provisioning");
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->firmware_applied_sha256, backup.firmware_applied_sha256,
           sizeof(out->firmware_applied_sha256));
    memcpy(out->workspace_slug, backup.workspace_slug, sizeof(out->workspace_slug));

    char url[384];
    int n = snprintf(url, sizeof(url), "%s/provision/device", CONFIG_IOTMER_PROVISION_API_URL);
    if (n < 0 || n >= (int)sizeof(url)) {
        *out = backup;
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        *out = backup;
        return ESP_ERR_NO_MEM;
    }
    (void)cJSON_AddStringToObject(root, "workspace_id", CONFIG_IOTMER_WORKSPACE_ID);
    if (strlen(CONFIG_IOTMER_TEMPLATE_ID) != 0) {
        (void)cJSON_AddStringToObject(root, "template_id", CONFIG_IOTMER_TEMPLATE_ID);
    }
    if (backup.device_id[0] != '\0') {
        (void)cJSON_AddStringToObject(root, "device_id", backup.device_id);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        *out = backup;
        return ESP_ERR_NO_MEM;
    }

    static char s_provision_json_resp[IOTMER_PROVISION_JSON_MAX];
    int http_status = 0;
    esp_err_t err = http_post_json(url, CONFIG_IOTMER_PROVISION_AUTH_CODE, body,
                                   s_provision_json_resp, sizeof(s_provision_json_resp),
                                   &http_status);
    cJSON_free(body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP post failed: %s", esp_err_to_name(err));
        *out = backup;
        return err;
    }
    if (http_status != 200 && http_status != 201) {
        ESP_LOGE(TAG, "Provision failed HTTP %d URL=%s response=%s",
                 http_status, url, s_provision_json_resp);
        *out = backup;
        return ESP_FAIL;
    }

    cJSON *jr = cJSON_Parse(s_provision_json_resp);
    if (!jr) {
        ESP_LOGE(TAG, "JSON parse failed (len=%zu first_128=%.128s)",
                 strlen(s_provision_json_resp), s_provision_json_resp);
        *out = backup;
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t e = ESP_OK;
    e = json_copy_string(jr, "device_id", out->device_id, sizeof(out->device_id));
    if (e != ESP_OK) {
        goto j_fail;
    }
    e = json_copy_string(jr, "device_key", out->device_key, sizeof(out->device_key));
    if (e != ESP_OK) {
        goto j_fail;
    }

    /* Optional firmware metadata (OTA + application use). Root or nested "data" object. */
    cJSON *inner = provision_inner_object(jr);

    esp_err_t fw_sha_e = json_copy_string(jr, "firmware_checksum_sha256",
                                          out->firmware_checksum_sha256,
                                          sizeof(out->firmware_checksum_sha256));
    if (fw_sha_e == ESP_ERR_NOT_FOUND && inner) {
        fw_sha_e = json_copy_string(inner, "firmware_checksum_sha256",
                                    out->firmware_checksum_sha256,
                                    sizeof(out->firmware_checksum_sha256));
    }
    if (fw_sha_e != ESP_OK && fw_sha_e != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "firmware_checksum_sha256 parse: %s", esp_err_to_name(fw_sha_e));
    }

    esp_err_t fw_size_e = json_copy_u32(jr, "firmware_size_bytes", &out->firmware_size_bytes);
    if (fw_size_e == ESP_ERR_NOT_FOUND && inner) {
        (void)json_copy_u32(inner, "firmware_size_bytes", &out->firmware_size_bytes);
    }

    esp_err_t fw_url_e = json_copy_string(jr, "firmware_url", out->firmware_url,
                                          sizeof(out->firmware_url));
    if (fw_url_e == ESP_ERR_NOT_FOUND && inner) {
        fw_url_e = json_copy_string(inner, "firmware_url", out->firmware_url,
                                     sizeof(out->firmware_url));
    }
    if (fw_url_e != ESP_OK && fw_url_e != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "firmware_url parse: %s (max len %zu)", esp_err_to_name(fw_url_e),
                 sizeof(out->firmware_url) - 1u);
    }
    if (fw_url_e == ESP_ERR_NOT_FOUND || fw_sha_e == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "provision JSON has no firmware_url and/or firmware_checksum_sha256 "
                      "(OTA will not run until the backend includes them)");
        ESP_LOGW(TAG, "provision body len=%zu prefix=%.400s", strlen(s_provision_json_resp),
                 s_provision_json_resp);
    } else if (inner != NULL &&
               (cJSON_GetObjectItemCaseSensitive(jr, "firmware_url") == NULL ||
                cJSON_GetObjectItemCaseSensitive(jr, "firmware_checksum_sha256") == NULL)) {
        ESP_LOGI(TAG, "firmware metadata read from nested \"data\" object");
    }

    e = json_copy_string(jr, "mqtt_host", out->mqtt_host, sizeof(out->mqtt_host));
    if (e != ESP_OK) {
        goto j_fail;
    }
    (void)json_copy_int(jr, "mqtt_port", &out->mqtt_port);
    (void)json_copy_bool(jr, "mqtt_tls", &out->mqtt_tls);
    e = json_copy_string(jr, "mqtt_username", out->mqtt_username, sizeof(out->mqtt_username));
    if (e != ESP_OK) {
        goto j_fail;
    }
    e = json_copy_string(jr, "mqtt_password", out->mqtt_password, sizeof(out->mqtt_password));
    if (e != ESP_OK) {
        goto j_fail;
    }

    {
        esp_err_t slug_e = json_copy_string(jr, "workspace_slug", out->workspace_slug,
                                            sizeof(out->workspace_slug));
        if (slug_e == ESP_ERR_NOT_FOUND && inner) {
            slug_e = json_copy_string(inner, "workspace_slug", out->workspace_slug,
                                       sizeof(out->workspace_slug));
        }
        if (slug_e != ESP_OK && slug_e != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "workspace_slug parse: %s", esp_err_to_name(slug_e));
        }
    }
    /* API should return workspace_slug; Kconfig is an optional fallback (tests / legacy). */
    if (out->workspace_slug[0] == '\0' && strlen(CONFIG_IOTMER_WORKSPACE_SLUG) != 0) {
        strncpy(out->workspace_slug, CONFIG_IOTMER_WORKSPACE_SLUG, sizeof(out->workspace_slug));
        out->workspace_slug[sizeof(out->workspace_slug) - 1] = '\0';
    }

    if (out->mqtt_port == 0) {
        out->mqtt_port = 8883;
    }

    {
        const size_t pwlen = strlen(out->mqtt_password);
        const bool had_pw = backup.mqtt_password[0] != '\0';
        const bool pw_changed =
            !had_pw || strcmp(backup.mqtt_password, out->mqtt_password) != 0;
        ESP_LOGI(TAG,
                 "MQTT (from provision): host=%s port=%d tls=%s username=%s "
                 "password_len=%zu password_%s",
                 out->mqtt_host, out->mqtt_port, out->mqtt_tls ? "yes" : "no",
                 out->mqtt_username, pwlen, pw_changed ? "changed_or_new" : "same_as_before");
    }

    if (https_performed) {
        *https_performed = true;
    }

    cJSON_Delete(jr);
    return ESP_OK;

j_fail:
    cJSON_Delete(jr);
    *out = backup;
    return e;
}

