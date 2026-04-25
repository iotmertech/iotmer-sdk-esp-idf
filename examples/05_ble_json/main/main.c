#include <string.h>

#include "cJSON.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "nvs_flash.h"

#include "iotmer_ble.h"
#include "iotmer_wifi.h"

static const char *TAG = "ble_json";

static void send_error(const char *code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "code", code != NULL ? code : "unknown");
    cJSON_AddStringToObject(root, "message", msg != NULL ? msg : "");

    char *out = cJSON_PrintUnformatted(root);
    if (out != NULL) {
        (void)iotmer_ble_send_json_str(out);
        cJSON_free(out);
    }
    cJSON_Delete(root);
}

static void send_ok(const char *type, const char *detail)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
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

    ESP_LOGI(TAG, "RX: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        send_error("bad_json", "Invalid JSON");
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        send_error("missing_type", "Missing 'type' field");
        return;
    }

    if (strcmp(type->valuestring, "ping") == 0) {
        cJSON_Delete(root);
        send_ok("pong", NULL);
        return;
    }

    if (strcmp(type->valuestring, "wifi.set") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
        if (!cJSON_IsString(ssid) || ssid->valuestring == NULL || ssid->valuestring[0] == '\0') {
            cJSON_Delete(root);
            send_error("bad_ssid", "Missing/empty 'ssid'");
            return;
        }
        if (!cJSON_IsString(pass) || pass->valuestring == NULL) {
            cJSON_Delete(root);
            send_error("bad_pass", "Missing 'pass'");
            return;
        }

        esp_err_t err = iotmer_wifi_set_credentials(ssid->valuestring, pass->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            send_error("nvs", esp_err_to_name(err));
            return;
        }

        // Apply immediately for demo purposes.
        (void)iotmer_wifi_reconnect();

        cJSON_Delete(root);
        send_ok("wifi.set.ok", NULL);
        return;
    }

    if (strcmp(type->valuestring, "wifi.clear") == 0) {
        esp_err_t err = iotmer_wifi_clear_credentials();
        cJSON_Delete(root);
        if (err != ESP_OK) {
            send_error("nvs", esp_err_to_name(err));
            return;
        }
        send_ok("wifi.clear.ok", NULL);
        return;
    }

    cJSON_Delete(root);
    send_error("unknown_type", "Unknown command");
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    iotmer_ble_cfg_t cfg = IOTMER_BLE_CFG_DEFAULT();
    cfg.on_rx_json = on_rx_json;

    ESP_ERROR_CHECK(iotmer_ble_init(&cfg));
    ESP_ERROR_CHECK(iotmer_ble_start());

    ESP_LOGI(TAG, "Ready. Send JSON over BLE RX characteristic.");
}

