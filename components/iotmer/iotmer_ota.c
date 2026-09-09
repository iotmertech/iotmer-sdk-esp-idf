/*
 * iotmer_ota.c — HTTPS OTA with streaming SHA256 verification.
 *
 * Do not use esp_https_ota_begin/perform here. That path calls esp_ota_begin
 * (full partition erase) after the TLS handshake, while the CDN body is already
 * flowing. On ESP32-C3 / ESP8685 the erase stalls the CPU, the lwIP window fills,
 * and Cloudflare/R2 resets the connection around ~50 KB — the download restarts
 * from byte 0 and a 1 MB image can take minutes or time out.
 *
 * Sequence:
 *   1. WIFI_PS_NONE (C3 USB/Wi‑Fi coexistence)
 *   2. esp_ota_begin(OTA_SIZE_UNKNOWN) — erase staging *before* TLS
 *   3. Single GET (pinned CA / crt_bundle via iotmer_tls_apply_http_client_config)
 *   4. 4 KB read → esp_ota_write + streaming SHA256
 *   5. Drop / EAGAIN → Range: bytes=<written>- (max 8 resumes, same OTA + SHA)
 *   6. Compare stream digest to firmware_checksum_sha256; mismatch → abort, no boot change
 *   7. esp_ota_end + esp_ota_set_boot_partition
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_wifi.h"
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

#ifndef CONFIG_IOTMER_OTA_TIMEOUT_MS
#define CONFIG_IOTMER_OTA_TIMEOUT_MS 180000
#endif

#define TAG "iotmer_ota"

#if CONFIG_IOTMER_AUTO_OTA

#define OTA_HTTP_RX           4096
#define OTA_HTTP_TX           2048
#define OTA_PROGRESS_BYTES    (128 * 1024)
#define OTA_MAX_RESUME        8
#define OTA_MAX_ATTEMPTS      3
#define OTA_RESUME_DELAY_MS   250
#define OTA_RETRY_DELAY_MS    400

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

static void digest_to_hex(const unsigned char digest[32], char hex[65])
{
    static const char lut[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex[i * 2]     = lut[digest[i] >> 4];
        hex[i * 2 + 1] = lut[digest[i] & 0x0F];
    }
    hex[64] = '\0';
}

static bool sha256_hex_eq(const unsigned char digest[32], const char *expected_hex)
{
    char hex[65];
    digest_to_hex(digest, hex);
    for (int i = 0; i < 64; ++i) {
        if (tolower((unsigned char)expected_hex[i]) != (unsigned char)hex[i]) {
            return false;
        }
    }
    return true;
}

static bool ota_error_is_fatal(esp_err_t err)
{
    return err == ESP_ERR_OTA_VALIDATE_FAILED ||
           err == ESP_ERR_INVALID_SIZE ||
           err == ESP_ERR_INVALID_ARG ||
           err == ESP_ERR_NOT_FOUND ||
           err == ESP_ERR_INVALID_VERSION;
}

static void http_discard(esp_http_client_handle_t *client)
{
    if (client && *client) {
        (void)esp_http_client_close(*client);
        (void)esp_http_client_cleanup(*client);
        *client = NULL;
    }
}

static void ota_abort_open(esp_ota_handle_t *ota, bool *open)
{
    if (open && *open) {
        (void)esp_ota_abort(*ota);
        *open = false;
        *ota = 0;
    }
}

static esp_err_t ota_staging_scrub(const esp_partition_t *part)
{
    esp_ota_handle_t tmp = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &tmp);
    if (err != ESP_OK) {
        return err;
    }
    (void)esp_ota_abort(tmp);
    return ESP_OK;
}

/*
 * Content-Range: bytes <start>-<end>/<total>  (total may be "*").
 */
static bool parse_content_range(const char *hdr, size_t *start, size_t *total)
{
    if (!hdr || !start || !total) {
        return false;
    }
    unsigned long s = 0, e = 0;
    char tot[32];
    if (sscanf(hdr, "bytes %lu-%lu/%31s", &s, &e, tot) < 3) {
        return false;
    }
    *start = (size_t)s;
    if (tot[0] == '*') {
        *total = 0;
    } else {
        *total = (size_t)strtoul(tot, NULL, 10);
    }
    (void)e;
    return true;
}

static esp_err_t http_get_open(const char *url, size_t range_from,
                               esp_http_client_handle_t *out,
                               int *status_out, int64_t *content_len_out)
{
    http_discard(out);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = CONFIG_IOTMER_OTA_TIMEOUT_MS,
        .buffer_size       = OTA_HTTP_RX,
        .buffer_size_tx    = OTA_HTTP_TX,
        .keep_alive_enable = false,
    };
    iotmer_tls_apply_http_client_config(&cfg);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    if (range_from > 0) {
        char range[40];
        snprintf(range, sizeof(range), "bytes=%u-", (unsigned)range_from);
        esp_err_t herr = esp_http_client_set_header(client, "Range", range);
        if (herr != ESP_OK) {
            (void)esp_http_client_cleanup(client);
            return herr;
        }
        ESP_LOGI(TAG, "OTA Range: bytes=%u-", (unsigned)range_from);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        (void)esp_http_client_cleanup(client);
        return err;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "HTTP fetch headers failed");
        (void)esp_http_client_close(client);
        (void)esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    *status_out = esp_http_client_get_status_code(client);
    *content_len_out = esp_http_client_get_content_length(client);
    *out = client;
    return ESP_OK;
}

/*
 * Read one HTTP body into the already-open OTA handle, hashing as we go.
 * Returns ESP_OK when this response is consumed and written==total (or EOF
 * with unknown length). ESP_ERR_HTTP_EAGAIN means Range-resume the same handle.
 */
static esp_err_t ota_read_body(esp_http_client_handle_t http,
                               const esp_partition_t *part,
                               esp_ota_handle_t ota,
                               iotmer_sha256_ctx_t *sha,
                               unsigned char *buf,
                               size_t skip,
                               size_t *written,
                               size_t total)
{
    size_t last_progress = *written / OTA_PROGRESS_BYTES;

    while (1) {
        int r = esp_http_client_read(http, (char *)buf, OTA_HTTP_RX);
        if (r < 0) {
            ESP_LOGW(TAG, "OTA read interrupted at %u/%u",
                     (unsigned)*written, (unsigned)total);
            return ESP_ERR_HTTP_EAGAIN;
        }
        if (r == 0) {
            bool complete = esp_http_client_is_complete_data_received(http);
            if (skip > 0 && complete) {
                ESP_LOGE(TAG, "OTA HTTP body shorter than Range skip (%u left)",
                         (unsigned)skip);
                return ESP_ERR_INVALID_SIZE;
            }
            if (total > 0 && *written >= total) {
                return ESP_OK;
            }
            if (complete && (total == 0 || *written >= total)) {
                return ESP_OK;
            }
            ESP_LOGW(TAG, "OTA incomplete at %u/%u (complete=%d)",
                     (unsigned)*written, (unsigned)total, (int)complete);
            return ESP_ERR_HTTP_EAGAIN;
        }

        const unsigned char *p = buf;
        size_t remain = (size_t)r;
        if (skip > 0) {
            size_t sk = (remain < skip) ? remain : skip;
            p += sk;
            remain -= sk;
            skip -= sk;
            if (remain == 0) {
                continue;
            }
        }

        if (*written + remain > part->size) {
            ESP_LOGE(TAG, "OTA image exceeds partition (%u + %u > %u)",
                     (unsigned)*written, (unsigned)remain, (unsigned)part->size);
            return ESP_ERR_INVALID_SIZE;
        }
        if (total > 0 && *written + remain > total) {
            ESP_LOGE(TAG, "OTA image exceeds Content-Length (%u + %u > %u)",
                     (unsigned)*written, (unsigned)remain, (unsigned)total);
            return ESP_ERR_INVALID_SIZE;
        }

        esp_err_t err = sha256_update(sha, p, remain);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SHA256 update failed");
            return err;
        }
        err = esp_ota_write(ota, p, remain);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            return err;
        }
        *written += remain;

        size_t step = *written / OTA_PROGRESS_BYTES;
        if (step != last_progress) {
            last_progress = step;
            ESP_LOGI(TAG, "OTA progress %d/%d", (int)*written, (int)total);
            vTaskDelay(1);
        }
    }
}

static esp_err_t ota_http_stream(const char *url,
                                 const esp_partition_t *part,
                                 esp_ota_handle_t ota,
                                 iotmer_sha256_ctx_t *sha,
                                 size_t *written_out,
                                 size_t *total_out)
{
    unsigned char *buf = (unsigned char *)malloc(OTA_HTTP_RX);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_handle_t http = NULL;
    size_t written = 0;
    size_t total = 0;
    int resumes = 0;
    esp_err_t err = ESP_FAIL;

    while (resumes <= OTA_MAX_RESUME) {
        int status = 0;
        int64_t content_len = -1;
        err = http_get_open(url, written, &http, &status, &content_len);
        if (err != ESP_OK) {
            if (written > 0 && resumes < OTA_MAX_RESUME) {
                resumes++;
                ESP_LOGW(TAG, "OTA reconnect %d/%d after %s",
                         resumes, OTA_MAX_RESUME, esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(OTA_RESUME_DELAY_MS));
                continue;
            }
            goto out;
        }

        if (status != 200 && status != 206) {
            ESP_LOGE(TAG, "OTA HTTP status %d", status);
            err = ESP_FAIL;
            goto out;
        }

        size_t skip = 0;
        if (status == 206) {
            char *cr = NULL;
            size_t cr_start = 0, cr_total = 0;
            if (esp_http_client_get_header(http, "Content-Range", &cr) == ESP_OK &&
                cr && parse_content_range(cr, &cr_start, &cr_total)) {
                if (cr_start != written) {
                    ESP_LOGE(TAG, "OTA Content-Range start %u != written %u",
                             (unsigned)cr_start, (unsigned)written);
                    err = ESP_ERR_INVALID_SIZE;
                    goto out;
                }
                if (cr_total > 0) {
                    total = cr_total;
                }
            } else if (content_len > 0) {
                total = written + (size_t)content_len;
            }
        } else {
            /* 200: full body. If this is a resume, skip bytes already hashed/written. */
            if (content_len > 0) {
                total = (size_t)content_len;
            }
            if (written > 0) {
                ESP_LOGW(TAG, "OTA Range ignored (HTTP 200) — skipping %u bytes",
                         (unsigned)written);
                skip = written;
                if (total > 0 && skip > total) {
                    err = ESP_ERR_INVALID_SIZE;
                    goto out;
                }
            }
        }

        if (total > part->size) {
            ESP_LOGE(TAG, "OTA Content-Length %u exceeds partition %u",
                     (unsigned)total, (unsigned)part->size);
            err = ESP_ERR_INVALID_SIZE;
            goto out;
        }

        err = ota_read_body(http, part, ota, sha, buf, skip, &written, total);
        http_discard(&http);

        if (err == ESP_OK) {
            if (total > 0 && written != total) {
                err = ESP_ERR_HTTP_EAGAIN;
            } else {
                break;
            }
        }
        if (err != ESP_ERR_HTTP_EAGAIN) {
            goto out;
        }
        if (resumes >= OTA_MAX_RESUME) {
            ESP_LOGE(TAG, "OTA resume limit (%d) at %u/%u",
                     OTA_MAX_RESUME, (unsigned)written, (unsigned)total);
            err = ESP_ERR_TIMEOUT;
            goto out;
        }
        resumes++;
        ESP_LOGW(TAG, "OTA resume %d/%d at %u/%u",
                 resumes, OTA_MAX_RESUME, (unsigned)written, (unsigned)total);
        vTaskDelay(pdMS_TO_TICKS(OTA_RESUME_DELAY_MS));
    }

    if (err == ESP_OK && total > 0 && written != total) {
        ESP_LOGE(TAG, "OTA size mismatch written=%u total=%u",
                 (unsigned)written, (unsigned)total);
        err = ESP_ERR_INVALID_SIZE;
    }

out:
    http_discard(&http);
    if (written_out) {
        *written_out = written;
    }
    if (total_out) {
        *total_out = total;
    }
    free(buf);
    return err;
}

static esp_err_t https_ota_download_and_verify(const iotmer_creds_t *creds)
{
    if (strlen(creds->firmware_checksum_sha256) != 64) {
        ESP_LOGE(TAG, "firmware_checksum_sha256 must be 64 hex chars");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_ps_type_t prev_ps = WIFI_PS_MIN_MODEM;
    bool restore_ps = (esp_wifi_get_ps(&prev_ps) == ESP_OK);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no OTA update partition");
        if (restore_ps) {
            (void)esp_wifi_set_ps(prev_ps);
        }
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t last = ESP_FAIL;

    for (int attempt = 1; attempt <= OTA_MAX_ATTEMPTS; ++attempt) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "OTA retry %d/%d (scrub staging)", attempt, OTA_MAX_ATTEMPTS);
            (void)ota_staging_scrub(part);
            vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));
        }

        ESP_LOGI(TAG, "OTA erase staging '%s' (%u bytes) before TLS",
                 part->label, (unsigned)part->size);

        esp_ota_handle_t ota = 0;
        bool ota_open = false;
        esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            last = err;
            if (ota_error_is_fatal(err)) {
                break;
            }
            continue;
        }
        ota_open = true;
        /* Let Wi‑Fi recover after a long erase on single-core chips. */
        vTaskDelay(pdMS_TO_TICKS(50));

        iotmer_sha256_ctx_t sha;
        err = sha256_begin(&sha);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SHA256 init failed");
            ota_abort_open(&ota, &ota_open);
            last = err;
            continue;
        }

        size_t written = 0, total = 0;
        err = ota_http_stream(creds->firmware_url, part, ota, &sha, &written, &total);
        if (err != ESP_OK) {
            sha256_cleanup(&sha);
            ota_abort_open(&ota, &ota_open);
            last = err;
            if (ota_error_is_fatal(err)) {
                break;
            }
            continue;
        }

        unsigned char digest[32];
        err = sha256_end(&sha, digest);
        sha256_cleanup(&sha);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SHA256 finish failed");
            ota_abort_open(&ota, &ota_open);
            last = err;
            continue;
        }

        if (!sha256_hex_eq(digest, creds->firmware_checksum_sha256)) {
            char hex[65];
            digest_to_hex(digest, hex);
            ESP_LOGE(TAG, "SHA256 mismatch: expected=%s got=%s",
                     creds->firmware_checksum_sha256, hex);
            ota_abort_open(&ota, &ota_open);
            last = ESP_ERR_OTA_VALIDATE_FAILED;
            break;
        }
        {
            char hex[65];
            digest_to_hex(digest, hex);
            ESP_LOGI(TAG, "SHA256 verified: %s (%u bytes)", hex, (unsigned)written);
        }

        err = esp_ota_end(ota);
        ota_open = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            last = err;
            break;
        }

        err = esp_ota_set_boot_partition(part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                     esp_err_to_name(err));
            last = err;
            break;
        }

        if (restore_ps) {
            (void)esp_wifi_set_ps(prev_ps);
        }
        return ESP_OK;
    }

    if (restore_ps) {
        (void)esp_wifi_set_ps(prev_ps);
    }
    return last;
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
