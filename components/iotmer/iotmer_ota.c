#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"

#include "sdkconfig.h"

#include "iotmer_internal.h"

#ifndef CONFIG_IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA
#define CONFIG_IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA 0
#endif

#define TAG "iotmer_ota"

#if CONFIG_IOTMER_AUTO_OTA

esp_err_t iotmer_ota_apply_if_needed(iotmer_creds_t *creds, bool after_https_provision)
{
    if (!creds) {
        return ESP_ERR_INVALID_ARG;
    }

    if (creds->firmware_url[0] == '\0') {
        if (creds->firmware_checksum_sha256[0] != '\0') {
            ESP_LOGW(TAG, "OTA skipped: firmware_url empty (checksum present — check URL length "
                          "or provision JSON)");
        } else {
            ESP_LOGI(TAG, "Auto-OTA inactive (no firmware_url / checksum from provision)");
        }
        return ESP_OK;
    }
    if (creds->firmware_checksum_sha256[0] == '\0') {
        ESP_LOGW(TAG, "firmware_url set but firmware_checksum_sha256 empty — skip OTA");
        return ESP_OK;
    }

    const bool ignore_sha_match =
        (bool)CONFIG_IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA || after_https_provision;

    if (!ignore_sha_match) {
        if (strcmp(creds->firmware_checksum_sha256, creds->firmware_applied_sha256) == 0) {
            ESP_LOGI(TAG, "OTA skipped (same SHA256 as last applied build)");
            return ESP_OK;
        }
    } else if (creds->firmware_applied_sha256[0] != '\0' &&
               strcmp(creds->firmware_checksum_sha256, creds->firmware_applied_sha256) == 0) {
        if (after_https_provision) {
            ESP_LOGI(TAG,
                     "OTA: same SHA as NVS — re-downloading because HTTPS provision just ran");
        } else {
            ESP_LOGI(TAG, "OTA: checksum matches NVS applied SHA — re-downloading anyway "
                          "(IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA)");
        }
    }

    esp_http_client_config_t http_cfg = {
        .url               = creds->firmware_url,
        .timeout_ms        = CONFIG_IOTMER_OTA_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 4096,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "Starting HTTPS OTA (expected sha256=%s)", creds->firmware_checksum_sha256);
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return err;
    }

    strncpy(creds->firmware_applied_sha256, creds->firmware_checksum_sha256,
            sizeof(creds->firmware_applied_sha256));
    creds->firmware_applied_sha256[sizeof(creds->firmware_applied_sha256) - 1] = '\0';

    err = iotmer_nvs_save_creds(creds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS save after OTA failed: %s — rebooting anyway",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA finished, rebooting into new firmware");
    esp_restart();
    return ESP_OK;
}

#else /* !CONFIG_IOTMER_AUTO_OTA */

esp_err_t iotmer_ota_apply_if_needed(iotmer_creds_t *creds, bool after_https_provision)
{
    (void)creds;
    (void)after_https_provision;
    return ESP_OK;
}

#endif /* CONFIG_IOTMER_AUTO_OTA */
