#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "cJSON.h"
#include "sdkconfig.h"

#include "iotmer_client.h"

#define TAG "iotmer_device_auth"

static esp_err_t http_post_bearer_json(const char *url, const char *bearer, const char *body,
                                       char *resp, size_t resp_len, int *http_status_out)
{
    if (!url || !bearer || !bearer[0] || !body || !resp || resp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t body_len = strlen(body);
    if (body_len > (size_t)INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = CONFIG_IOTMER_PROVISION_TIMEOUT_MS,
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }

    char auth_hdr[448];
    int an = snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", bearer);
    if (an < 0 || an >= (int)sizeof(auth_hdr)) {
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(c, "Content-Type", "application/json");
    if (err != ESP_OK) {
        goto out;
    }
    err = esp_http_client_set_header(c, "Authorization", auth_hdr);
    if (err != ESP_OK) {
        goto out;
    }

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

    if (esp_http_client_fetch_headers(c) < 0) {
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
    err         = ESP_OK;

out:
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t iotmer_device_auth_bind_claim(const iotmer_creds_t *creds, const char *claim_code)
{
    if (!creds || !claim_code || claim_code[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (creds->device_http_token[0] == '\0') {
        ESP_LOGE(TAG, "bind-claim: device_http_token missing (run HTTPS provision first)");
        return ESP_ERR_INVALID_STATE;
    }

    char url[512];
    int n = snprintf(url, sizeof(url), "%s/devices/auth/bind-claim", CONFIG_IOTMER_PROVISION_API_URL);
    if (n < 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    (void)cJSON_AddStringToObject(root, "claim_code", claim_code);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    static char s_bind_resp[1024];
    int        http_status = 0;
    esp_err_t  err         = http_post_bearer_json(url, creds->device_http_token, body, s_bind_resp,
                                                   sizeof(s_bind_resp), &http_status);
    cJSON_free(body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bind-claim HTTP failed: %s", esp_err_to_name(err));
        return err;
    }

    if (http_status == 200) {
        cJSON *jr = cJSON_Parse(s_bind_resp);
        if (jr) {
            cJSON *ok = cJSON_GetObjectItemCaseSensitive(jr, "ok");
            if (cJSON_IsBool(ok) && cJSON_IsTrue(ok)) {
                cJSON_Delete(jr);
                return ESP_OK;
            }
            cJSON_Delete(jr);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGW(TAG, "bind-claim failed HTTP %d (body redacted in production; len=%zu)", http_status,
             strlen(s_bind_resp));

    if (http_status == 401) {
        return ESP_ERR_INVALID_ARG;
    }
    if (http_status == 409 || http_status == 410) {
        return ESP_ERR_INVALID_STATE;
    }
    if (http_status == 400 || http_status == 403) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_FAIL;
}
