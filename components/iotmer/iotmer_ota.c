/*
 * iotmer_ota.c — HTTPS OTA with end-to-end SHA256 verification.
 *
 * Uses the advanced esp_https_ota_begin/perform/finish API. After the download
 * completes (before the new image is activated), the bytes written to the
 * update partition are read back and hashed with mbedtls SHA256; the digest is
 * compared against the provision `firmware_checksum_sha256`. On mismatch the
 * OTA is aborted — a corrupt or wrong file served by the CDN never boots.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * ESP-IDF >= 6.0 ships MbedTLS 4 / TF-PSA-Crypto where the legacy
 * mbedtls/sha256.h header is gone; use the PSA Crypto API there.
 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "psa/crypto.h"
#define IOTMER_OTA_SHA_USE_PSA 1
#else
#include "mbedtls/sha256.h"
#define IOTMER_OTA_SHA_USE_PSA 0
#endif

#include "sdkconfig.h"

#include "iotmer_internal.h"

#ifndef CONFIG_IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA
#define CONFIG_IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA 0
#endif

#define TAG "iotmer_ota"

#if CONFIG_IOTMER_AUTO_OTA

/* Flash read-back chunk for the post-download hash pass. */
#define OTA_SHA_READ_CHUNK 4096

/* Thin SHA256 streaming wrapper over PSA Crypto (IDF >= 6) or legacy mbedtls. */
#if IOTMER_OTA_SHA_USE_PSA
typedef psa_hash_operation_t iotmer_sha256_ctx_t;

static esp_err_t sha256_begin(iotmer_sha256_ctx_t *ctx)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    *ctx = psa_hash_operation_init();
    return (psa_hash_setup(ctx, PSA_ALG_SHA_256) == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t sha256_update(iotmer_sha256_ctx_t *ctx, const unsigned char *data, size_t len)
{
    return (psa_hash_update(ctx, data, len) == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t sha256_end(iotmer_sha256_ctx_t *ctx, unsigned char digest[32])
{
    size_t out_len = 0;
    return (psa_hash_finish(ctx, digest, 32, &out_len) == PSA_SUCCESS && out_len == 32)
               ? ESP_OK
               : ESP_FAIL;
}

static void sha256_cleanup(iotmer_sha256_ctx_t *ctx)
{
    (void)psa_hash_abort(ctx);
}
#else
typedef mbedtls_sha256_context iotmer_sha256_ctx_t;

static esp_err_t sha256_begin(iotmer_sha256_ctx_t *ctx)
{
    mbedtls_sha256_init(ctx);
    return (mbedtls_sha256_starts(ctx, 0 /* SHA-256 */) == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t sha256_update(iotmer_sha256_ctx_t *ctx, const unsigned char *data, size_t len)
{
    return (mbedtls_sha256_update(ctx, data, len) == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t sha256_end(iotmer_sha256_ctx_t *ctx, unsigned char digest[32])
{
    return (mbedtls_sha256_finish(ctx, digest) == 0) ? ESP_OK : ESP_FAIL;
}

static void sha256_cleanup(iotmer_sha256_ctx_t *ctx)
{
    mbedtls_sha256_free(ctx);
}
#endif /* IOTMER_OTA_SHA_USE_PSA */

/*
 * Read @p image_len bytes back from @p part and compare their SHA256 against
 * @p expected_hex (64 lowercase/uppercase hex chars).
 */
static esp_err_t verify_partition_sha256(const esp_partition_t *part,
                                         size_t image_len,
                                         const char *expected_hex)
{
    if (!part || image_len == 0 || !expected_hex || strlen(expected_hex) != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (image_len > part->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    unsigned char *buf = (unsigned char *)malloc(OTA_SHA_READ_CHUNK);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    iotmer_sha256_ctx_t sha;
    esp_err_t err = sha256_begin(&sha);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    for (size_t off = 0; off < image_len; ) {
        size_t n = image_len - off;
        if (n > OTA_SHA_READ_CHUNK) {
            n = OTA_SHA_READ_CHUNK;
        }
        err = esp_partition_read(part, off, buf, n);
        if (err != ESP_OK) {
            goto out;
        }
        err = sha256_update(&sha, buf, n);
        if (err != ESP_OK) {
            goto out;
        }
        off += n;
        /* Yield periodically so IDLE/WiFi keep running on single-core chips. */
        if ((off % (OTA_SHA_READ_CHUNK * 16)) == 0) {
            vTaskDelay(1);
        }
    }

    unsigned char digest[32];
    err = sha256_end(&sha, digest);
    if (err != ESP_OK) {
        goto out;
    }

    char hex[65];
    for (int i = 0; i < 32; ++i) {
        static const char lut[] = "0123456789abcdef";
        hex[i * 2]     = lut[digest[i] >> 4];
        hex[i * 2 + 1] = lut[digest[i] & 0x0F];
    }
    hex[64] = '\0';

    bool match = true;
    for (int i = 0; i < 64; ++i) {
        if (tolower((unsigned char)expected_hex[i]) != hex[i]) {
            match = false;
            break;
        }
    }

    if (!match) {
        ESP_LOGE(TAG, "SHA256 mismatch: expected=%s got=%s", expected_hex, hex);
        err = ESP_ERR_OTA_VALIDATE_FAILED;
    } else {
        ESP_LOGI(TAG, "SHA256 verified: %s", hex);
    }

out:
    sha256_cleanup(&sha);
    free(buf);
    return err;
}

static esp_err_t https_ota_download_and_verify(const iotmer_creds_t *creds)
{
    esp_http_client_config_t http_cfg = {
        .url               = creds->firmware_url,
        .timeout_ms        = CONFIG_IOTMER_OTA_TIMEOUT_MS,
        .buffer_size       = 4096,
    };
    iotmer_tls_apply_http_client_config(&http_cfg);

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        /* esp_https_ota_perform reads+writes one chunk per call. */
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        (void)esp_https_ota_abort(handle);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA download incomplete — aborting");
        (void)esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    int image_len = esp_https_ota_get_image_len_read(handle);

    /*
     * Verify the downloaded bytes against the provision checksum *before*
     * activating the image. esp_https_ota writes to the next update partition
     * (same one esp_ota_get_next_update_partition reports for the running app).
     */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part || image_len <= 0) {
        ESP_LOGE(TAG, "cannot locate update partition for SHA verification");
        (void)esp_https_ota_abort(handle);
        return ESP_FAIL;
    }

    err = verify_partition_sha256(update_part, (size_t)image_len,
                                  creds->firmware_checksum_sha256);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image rejected (checksum verification failed: %s)",
                 esp_err_to_name(err));
        (void)esp_https_ota_abort(handle);
        return err;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

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

    ESP_LOGI(TAG, "Starting HTTPS OTA (expected sha256=%s)", creds->firmware_checksum_sha256);
    esp_err_t err = https_ota_download_and_verify(creds);
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
