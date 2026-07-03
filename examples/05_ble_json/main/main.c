#include <string.h>

#include "cJSON.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iotmer_ble.h"
#include "iotmer_client.h"
#include "iotmer_wifi.h"

static const char *TAG = "ble_json";

static void send_error_rid(const char *rid, const char *code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    if (rid != NULL && rid[0] != '\0') {
        cJSON_AddStringToObject(root, "rid", rid);
    }
    cJSON_AddStringToObject(root, "code", code != NULL ? code : "unknown");
    cJSON_AddStringToObject(root, "message", msg != NULL ? msg : "");

    char *out = cJSON_PrintUnformatted(root);
    if (out != NULL) {
        (void)iotmer_ble_send_json_str(out);
        cJSON_free(out);
    }
    cJSON_Delete(root);
}

static void send_error(const char *code, const char *msg)
{
    send_error_rid(NULL, code, msg);
}

static void send_ok_rid(const char *rid, const char *type, const char *detail)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (rid != NULL && rid[0] != '\0') {
        cJSON_AddStringToObject(root, "rid", rid);
    }
    if (detail != NULL) {
        cJSON_AddStringToObject(root, "detail", detail);
    }
    char *out = cJSON_PrintUnformatted(root);
    if (out != NULL) {
        (void)iotmer_ble_send_json_str(out);
        cJSON_free(out);
    }
    cJSON_Delete(root);
}

typedef struct {
    char rid[64];
    char claim_code[192];
} claim_job_args_t;

static TaskHandle_t s_claim_task;

static void claim_bind_task(void *param)
{
    claim_job_args_t *args = (claim_job_args_t *)param;

    const char *rid = (args && args->rid[0] != '\0') ? args->rid : NULL;
    send_ok_rid(rid, "claim.bind.start", NULL);

    esp_err_t err = iotmer_wifi_reconnect();
    if (err != ESP_OK) {
        send_error_rid(rid, "wifi", esp_err_to_name(err));
        goto out;
    }
    send_ok_rid(rid, "wifi.connect.ok", NULL);

    /* Load device_http_token (NVS key: "dht") from stored IOTMER credentials. */
    iotmer_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    err = iotmer_nvs_load_creds(&creds);
    if (err != ESP_OK) {
        send_error_rid(rid, "creds", esp_err_to_name(err));
        goto out;
    }
    if (creds.device_http_token[0] == '\0') {
        send_error_rid(rid, "dht_missing",
                       "device_http_token missing. Run HTTPS provision once (01_provisioning) or set it in NVS.");
        goto out;
    }
    send_ok_rid(rid, "creds.ok", NULL);

    /* Bind the device to the mobile identity using the claim code. */
    err = iotmer_device_auth_bind_claim(&creds, args->claim_code);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim.bind failed: %s", esp_err_to_name(err));
        send_error_rid(rid, "claim.bind", esp_err_to_name(err));
        goto out;
    }
    send_ok_rid(rid, "claim.bind.ok", NULL);

out:
    if (args) {
        /* Best-effort: clear sensitive data before free. */
        memset(args, 0, sizeof(*args));
        free(args);
    }
    s_claim_task = NULL;
    vTaskDelete(NULL);
}

static void on_rx_json(void *user_ctx, const uint8_t *data, size_t len)
{
    (void)user_ctx;

    if (data == NULL || len == 0) {
        return;
    }

    // cJSON expects NUL-terminated input.
    char buf[513];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        send_error("bad_json", "Invalid JSON");
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "rid");
    const char *rid_s = (cJSON_IsString(rid) && rid->valuestring) ? rid->valuestring : NULL;
    char rid_copy[64] = {0};
    if (rid_s != NULL) {
        strncpy(rid_copy, rid_s, sizeof(rid_copy) - 1);
        rid_s = rid_copy; /* safe after cJSON_Delete(root) */
    }
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        send_error_rid(rid_s, "missing_type", "Missing 'type' field");
        return;
    }

    /* Do not log secrets (WiFi password / claim_code). Log only the command type. */
    ESP_LOGI(TAG, "RX type=%s", type->valuestring);

    if (strcmp(type->valuestring, "ping") == 0) {
        send_ok_rid(rid_s, "pong", NULL);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "wifi.set") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
        const cJSON *claim = cJSON_GetObjectItemCaseSensitive(root, "claim_code");
        const char *claim_s = (cJSON_IsString(claim) && claim->valuestring) ? claim->valuestring : NULL;
        char claim_copy[192] = {0};
        if (claim_s != NULL) {
            strncpy(claim_copy, claim_s, sizeof(claim_copy) - 1);
            claim_s = claim_copy; /* safe after cJSON_Delete(root) */
        }
        if (!cJSON_IsString(ssid) || ssid->valuestring == NULL || ssid->valuestring[0] == '\0') {
            cJSON_Delete(root);
            send_error_rid(rid_s, "bad_ssid", "Missing/empty 'ssid'");
            return;
        }
        if (!cJSON_IsString(pass) || pass->valuestring == NULL) {
            cJSON_Delete(root);
            send_error_rid(rid_s, "bad_pass", "Missing 'pass'");
            return;
        }

        esp_err_t err = iotmer_wifi_set_credentials(ssid->valuestring, pass->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            send_error_rid(rid_s, "nvs", esp_err_to_name(err));
            return;
        }

        /* ACK quickly (do not block BLE RX handling). */
        send_ok_rid(rid_s, "wifi.set.ok", NULL);

        /* Optional: if claim_code is provided, run WiFi reconnect + HTTPS provision + bind-claim. */
        if (claim_s != NULL && claim_s[0] != '\0') {
            if (s_claim_task != NULL) {
                cJSON_Delete(root);
                send_error_rid(rid_s, "busy", "Another claim job is running");
                return;
            }

            claim_job_args_t *args = (claim_job_args_t *)calloc(1, sizeof(*args));
            if (args == NULL) {
                cJSON_Delete(root);
                send_error_rid(rid_s, "no_mem", "Out of memory");
                return;
            }

            if (rid_s != NULL) {
                strncpy(args->rid, rid_s, sizeof(args->rid) - 1);
            }
            strncpy(args->claim_code, claim_s, sizeof(args->claim_code) - 1);

            if (xTaskCreate(claim_bind_task, "claim_bind", 6144, args, 5, &s_claim_task) != pdPASS) {
                memset(args, 0, sizeof(*args));
                free(args);
                s_claim_task = NULL;
                cJSON_Delete(root);
                send_error_rid(rid_s, "task", "Failed to start background task");
                return;
            }
        }

        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "wifi.clear") == 0) {
        esp_err_t err = iotmer_wifi_clear_credentials();
        cJSON_Delete(root);
        if (err != ESP_OK) {
            send_error_rid(rid_s, "nvs", esp_err_to_name(err));
            return;
        }
        send_ok_rid(rid_s, "wifi.clear.ok", NULL);
        return;
    }

    cJSON_Delete(root);
    send_error_rid(rid_s, "unknown_type", "Unknown command");
}

static void on_ble_connect(void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "BLE central connected");
}

static void on_ble_disconnect(void *user_ctx, int reason)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "BLE central disconnected (reason=%d)", reason);
}

static void on_ble_mtu(void *user_ctx, uint16_t mtu)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "BLE ATT MTU=%u", (unsigned)mtu);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    iotmer_ble_cfg_t cfg = IOTMER_BLE_CFG_DEFAULT();
    cfg.device_name   = "MER-Setup";      /* optional; NULL => MAC-derived name */
    cfg.on_rx_json    = on_rx_json;
    cfg.on_connect    = on_ble_connect;
    cfg.on_disconnect = on_ble_disconnect;
    cfg.on_mtu        = on_ble_mtu;
    /* Dispatch RX on a worker task so the parse/claim work is off the NimBLE host context. */
    cfg.rx_queue_len  = 4;
    cfg.rx_task_stack = 6144;

    ESP_ERROR_CHECK(iotmer_ble_init(&cfg));
    ESP_ERROR_CHECK(iotmer_ble_start());

    ESP_LOGI(TAG, "Ready. Send JSON over BLE RX characteristic.");
}

