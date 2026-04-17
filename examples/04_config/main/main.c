#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iotmer_client.h"
#include "iotmer_config.h"

static const char *TAG = "04_config";

static iotmer_client_t s_client;
static iotmer_config_ctx_t s_cfg;
/* Lower half: gzip accum; upper half: JSON staging + inflated config (see iotmer_config.h). */
static uint8_t s_cfg_buf[65536];

static void on_cfg_event(void *user, const iotmer_config_event_t *ev)
{
    (void)user;
    switch (ev->type) {
    case IOTMER_CONFIG_EV_META:
        ESP_LOGI(TAG, "META version=%u sha=%s bytes_hint=%u", (unsigned)ev->u.meta.version,
                 ev->u.meta.sha256_hex, (unsigned)ev->u.meta.bytes_hint);
        if (s_cfg.have_valid && s_cfg.have_version == ev->u.meta.version &&
            strncmp(s_cfg.have_sha_hex, ev->u.meta.sha256_hex, sizeof(s_cfg.have_sha_hex)) == 0) {
            ESP_LOGD(TAG, "META matches applied config — skip config/get");
            break;
        }
        {
            char rid[IOTMER_CONFIG_RID_LEN];
            esp_err_t e = iotmer_config_request(&s_cfg, &s_client, 4096U, 1024U * 1024U, rid);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "config/get publish failed: %s", esp_err_to_name(e));
            } else {
                ESP_LOGI(TAG, "config/get sent rid=%s", rid);
            }
        }
        break;
    case IOTMER_CONFIG_EV_CONFIG_JSON:
        ESP_LOGI(TAG, "CONFIG ok rid=%s ver=%u sha=%s json_len=%u", ev->rid,
                 (unsigned)ev->u.config.version, ev->u.config.sha256_hex,
                 (unsigned)ev->u.config.json_len);
        if (ev->u.config.json_len > 0U) {
            int preview = (int)ev->u.config.json_len;
            if (preview > 200) {
                preview = 200;
            }
            ESP_LOGI(TAG, "json preview: %.*s", preview, (const char *)ev->u.config.json_utf8);
        }
        (void)iotmer_config_publish_status(&s_client, ev->rid, true, ev->u.config.version,
                                            ev->u.config.sha256_hex, NULL, NULL);
        iotmer_config_set_have(&s_cfg, ev->u.config.version, ev->u.config.sha256_hex);
        break;
    case IOTMER_CONFIG_EV_RESP_ERROR:
        ESP_LOGW(TAG, "RESP err rid=%s code=%s msg=%s retryable=%d", ev->rid,
                 ev->u.resp_err.code, ev->u.resp_err.message, (int)ev->u.resp_err.retryable);
        /* `config/status` applied=false needs version+sha256; include them when cloud adds to err payload. */
        break;
    case IOTMER_CONFIG_EV_FAIL:
        ESP_LOGE(TAG, "FAIL: %s", ev->u.fail.message);
        break;
    default:
        break;
    }
}

static void on_config_mqtt(const char *topic, const char *payload, int len, void *ctx)
{
    iotmer_client_t *cl = (iotmer_client_t *)ctx;
    esp_err_t e = iotmer_config_on_mqtt(&s_cfg, cl, topic, payload, len, on_cfg_event, NULL);
    if (e != ESP_OK && e != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "iotmer_config_on_mqtt: %s", esp_err_to_name(e));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    iotmer_config_ctx_init(&s_cfg, s_cfg_buf, sizeof(s_cfg_buf));

    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    err = iotmer_init(&s_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = iotmer_connect(&s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_connect failed: %s", esp_err_to_name(err));
        return;
    }

    while (!s_client.connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    err = iotmer_subscribe_config(&s_client, on_config_mqtt, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "subscribe config failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Subscribed to config/#; waiting for config/meta (retained) or config/resp…");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
