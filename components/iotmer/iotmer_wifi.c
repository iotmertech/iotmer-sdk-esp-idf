/*
 * iotmer_wifi.c — WiFi STA helper.
 *
 * iotmer_wifi_connect() is idempotent: calling it a second time after a
 * successful connection is a no-op (returns ESP_OK immediately).
 *
 * Internally it uses an event group to block until an IP is acquired or
 * the connection attempt fails.  The retry limit (WIFI_MAX_RETRY) caps the
 * number of reconnect attempts before giving up and returning ESP_FAIL.
 *
 * Note: nvs_flash_init() and esp_event_loop_create_default() are the
 * application's responsibility; this module calls them defensively and
 * tolerates ESP_ERR_INVALID_STATE (already initialised).
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "iotmer_internal.h"
#include "iotmer_wifi.h"

#define TAG "iotmer_wifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     10
#define WIFI_CONNECT_TIMEOUT_MS 30000

/* Keep keys ≤15 chars (ESP-IDF NVS limit). */
#define IOTMER_WIFI_NVS_KEY_SSID "wifi_ssid"
#define IOTMER_WIFI_NVS_KEY_PASS "wifi_pass"

static EventGroupHandle_t s_event_group;
static int                s_retry_num;
static bool               s_inited;
static bool               s_connected;

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

static esp_err_t wifi_creds_load_from_nvs(char *ssid_out,
                                         size_t ssid_out_len,
                                         char *pass_out,
                                         size_t pass_out_len)
{
    if (!ssid_out || ssid_out_len == 0 || !pass_out || pass_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid_out[0] = '\0';
    pass_out[0] = '\0';

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CONFIG_IOTMER_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t e1 = nvs_get_str_safe(h, IOTMER_WIFI_NVS_KEY_SSID, ssid_out, ssid_out_len);
    esp_err_t e2 = nvs_get_str_safe(h, IOTMER_WIFI_NVS_KEY_PASS, pass_out, pass_out_len);
    nvs_close(h);

    if (e1 != ESP_OK || e2 != ESP_OK || ssid_out[0] == '\0') {
        ssid_out[0] = '\0';
        pass_out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void wifi_creds_save_to_nvs_if_possible(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0' || !pass) {
        return;
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CONFIG_IOTMER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return;
    }

    bool changed = false;

    char have_ssid[sizeof(((wifi_config_t *)0)->sta.ssid)] = {0};
    char have_pass[sizeof(((wifi_config_t *)0)->sta.password)] = {0};
    esp_err_t e1 = nvs_get_str_safe(h, IOTMER_WIFI_NVS_KEY_SSID, have_ssid, sizeof(have_ssid));
    esp_err_t e2 = nvs_get_str_safe(h, IOTMER_WIFI_NVS_KEY_PASS, have_pass, sizeof(have_pass));

    if (e1 != ESP_OK || strcmp(have_ssid, ssid) != 0) {
        if (nvs_set_str(h, IOTMER_WIFI_NVS_KEY_SSID, ssid) == ESP_OK) {
            changed = true;
        }
    }
    if (e2 != ESP_OK || strcmp(have_pass, pass) != 0) {
        if (nvs_set_str(h, IOTMER_WIFI_NVS_KEY_PASS, pass) == ESP_OK) {
            changed = true;
        }
    }

    if (changed) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

esp_err_t iotmer_wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CONFIG_IOTMER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, IOTMER_WIFI_NVS_KEY_SSID, ssid);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(h, IOTMER_WIFI_NVS_KEY_PASS, password);
    if (err != ESP_OK) goto out;

    err = nvs_commit(h);

out:
    nvs_close(h);
    return err;
}

esp_err_t iotmer_wifi_get_credentials(char *ssid_out, size_t ssid_out_len,
                                      char *password_out, size_t password_out_len)
{
    return wifi_creds_load_from_nvs(ssid_out, ssid_out_len, password_out, password_out_len);
}

esp_err_t iotmer_wifi_clear_credentials(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CONFIG_IOTMER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* Ignore NOT_FOUND to keep this idempotent. */
    esp_err_t e1 = nvs_erase_key(h, IOTMER_WIFI_NVS_KEY_SSID);
    esp_err_t e2 = nvs_erase_key(h, IOTMER_WIFI_NVS_KEY_PASS);
    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) err = e1;
    if (err == ESP_OK && e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) err = e2;

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t iotmer_wifi_reconnect(void)
{
    /* Force a reconnect on next connect call. */
    s_connected = false;

    /* Stop WiFi if it's running (ignore state errors). */
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        /* ignore */
    }
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        /* ignore */
    }

    return iotmer_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d",
                     s_retry_num, WIFI_MAX_RETRY);
            (void)esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi connect failed after %d retries", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_once(void)
{
    if (s_inited) return ESP_OK;

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        ESP_LOGE(TAG, "EventGroup alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* esp_netif_init and esp_event_loop_create_default are tolerant of being
     * called more than once: they return ESP_ERR_INVALID_STATE which we treat
     * as "already done". */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    s_inited = true;
    return ESP_OK;
}

esp_err_t iotmer_wifi_connect(void)
{
    /* Idempotent: skip if already connected. */
    if (s_connected) return ESP_OK;

    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return err;
    }

    char ssid[sizeof(((wifi_config_t *)0)->sta.ssid)] = {0};
    char pass[sizeof(((wifi_config_t *)0)->sta.password)] = {0};

    /* Prefer WiFi creds persisted in NVS so OTA firmware doesn't need menuconfig. */
    if (wifi_creds_load_from_nvs(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials loaded from NVS (ns=%s)", CONFIG_IOTMER_NVS_NAMESPACE);
    } else {
        /* Fall back to Kconfig values (menuconfig). */
        if (strlen(CONFIG_IOTMER_WIFI_SSID) == 0) {
            ESP_LOGE(TAG, "CONFIG_IOTMER_WIFI_SSID is empty — set it in menuconfig (or store WiFi in NVS)");
            return ESP_ERR_INVALID_STATE;
        }
        strncpy(ssid, CONFIG_IOTMER_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, CONFIG_IOTMER_WIFI_PASSWORD, sizeof(pass) - 1);
        wifi_creds_save_to_nvs_if_possible(ssid, pass);
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable    = true;
    wifi_cfg.sta.pmf_cfg.required   = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) return err;

    /* Clear stale bits before starting (handles repeated calls after failure). */
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE  /* clear on exit */,
        pdFALSE /* wait for any bit */,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    if (bits & WIFI_FAIL_BIT)      return ESP_FAIL;

    ESP_LOGE(TAG, "WiFi connect timed out after %d ms", WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}
