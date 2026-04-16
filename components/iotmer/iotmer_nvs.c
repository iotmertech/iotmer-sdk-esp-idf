#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "sdkconfig.h"

#include "iotmer_internal.h"

#define TAG "iotmer_nvs"

/* NVS key names must be ≤15 characters (ESP-IDF). */
#define NVS_KEY_OTA_APPLIED_SHA "ota_sha" /* was fw_applied_sha256 (17) → KEY_TOO_LONG */
#define NVS_KEY_FW_DONE_SHA256 "fw_done_sha256"

_Static_assert(sizeof(NVS_KEY_OTA_APPLIED_SHA) <= 16, "NVS key ota_sha");
_Static_assert(sizeof(NVS_KEY_FW_DONE_SHA256) <= 16, "NVS key fw_done_sha256");
_Static_assert(sizeof("workspace_slug") <= 16, "NVS key workspace_slug");
_Static_assert(sizeof("mqtt_username") <= 16, "NVS key mqtt_username");
_Static_assert(sizeof("mqtt_password") <= 16, "NVS key mqtt_password");

static const char *ns(void)
{
    return CONFIG_IOTMER_NVS_NAMESPACE;
}

static esp_err_t nvs_get_str_safe(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    if (!key || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t required = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &required);
    if (err != ESP_OK) {
        return err;
    }
    if (required == 0 || required > out_len) {
        return ESP_ERR_NO_MEM;
    }
    return nvs_get_str(h, key, out, &required);
}

esp_err_t iotmer_nvs_load_creds(iotmer_creds_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(ns(), NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t e1 = nvs_get_str_safe(h, "device_id", out->device_id, sizeof(out->device_id));
    esp_err_t e2 = nvs_get_str_safe(h, "device_key", out->device_key, sizeof(out->device_key));
    (void)nvs_get_str_safe(h, "fw_sha256", out->firmware_checksum_sha256,
                           sizeof(out->firmware_checksum_sha256));
    (void)nvs_get_u32(h, "fw_size", &out->firmware_size_bytes);
    (void)nvs_get_str_safe(h, "fw_url", out->firmware_url, sizeof(out->firmware_url));
    if (nvs_get_str_safe(h, NVS_KEY_OTA_APPLIED_SHA, out->firmware_applied_sha256,
                         sizeof(out->firmware_applied_sha256)) != ESP_OK) {
        (void)nvs_get_str_safe(h, NVS_KEY_FW_DONE_SHA256, out->firmware_applied_sha256,
                               sizeof(out->firmware_applied_sha256));
    }
    esp_err_t e3 = nvs_get_str_safe(h, "mqtt_host", out->mqtt_host, sizeof(out->mqtt_host));
    esp_err_t e4 = nvs_get_str_safe(h, "mqtt_username", out->mqtt_username, sizeof(out->mqtt_username));
    esp_err_t e5 = nvs_get_str_safe(h, "mqtt_password", out->mqtt_password, sizeof(out->mqtt_password));

    int32_t port = 0;
    int8_t tls = 0;
    (void)nvs_get_i32(h, "mqtt_port", &port);
    (void)nvs_get_i8(h, "mqtt_tls", &tls);
    out->mqtt_port = (int)port;
    out->mqtt_tls = tls ? true : false;

    (void)nvs_get_str_safe(h, "workspace_slug", out->workspace_slug, sizeof(out->workspace_slug));

    nvs_close(h);

    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK || e5 != ESP_OK) {
        ESP_LOGW(TAG, "credentials missing in NVS");
        return ESP_ERR_NOT_FOUND;
    }
    if (out->mqtt_port == 0) {
        out->mqtt_port = 8883;
    }

    return ESP_OK;
}

esp_err_t iotmer_nvs_save_creds(const iotmer_creds_t *creds)
{
    if (!creds) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(ns(), NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, "device_id", creds->device_id);
    if (err != ESP_OK) goto out;
    err = nvs_set_str(h, "device_key", creds->device_key);
    if (err != ESP_OK) goto out;
    if (creds->firmware_checksum_sha256[0] != '\0') {
        err = nvs_set_str(h, "fw_sha256", creds->firmware_checksum_sha256);
        if (err != ESP_OK) goto out;
    }
    if (creds->firmware_size_bytes != 0) {
        err = nvs_set_u32(h, "fw_size", creds->firmware_size_bytes);
        if (err != ESP_OK) goto out;
    }
    if (creds->firmware_url[0] != '\0') {
        err = nvs_set_str(h, "fw_url", creds->firmware_url);
        if (err != ESP_OK) goto out;
    }
    if (creds->firmware_applied_sha256[0] != '\0') {
        err = nvs_set_str(h, NVS_KEY_OTA_APPLIED_SHA, creds->firmware_applied_sha256);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS set %s failed: %s", NVS_KEY_OTA_APPLIED_SHA, esp_err_to_name(err));
            goto out;
        }
    }
    err = nvs_set_str(h, "mqtt_host", creds->mqtt_host);
    if (err != ESP_OK) goto out;
    err = nvs_set_i32(h, "mqtt_port", creds->mqtt_port);
    if (err != ESP_OK) goto out;
    err = nvs_set_i8(h, "mqtt_tls", creds->mqtt_tls ? 1 : 0);
    if (err != ESP_OK) goto out;
    err = nvs_set_str(h, "mqtt_username", creds->mqtt_username);
    if (err != ESP_OK) goto out;
    err = nvs_set_str(h, "mqtt_password", creds->mqtt_password);
    if (err != ESP_OK) goto out;
    if (creds->workspace_slug[0] != '\0') {
        err = nvs_set_str(h, "workspace_slug", creds->workspace_slug);
        if (err != ESP_OK) goto out;
    }

    err = nvs_commit(h);

out:
    nvs_close(h);
    return err;
}

