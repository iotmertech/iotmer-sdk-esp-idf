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
#include "sdkconfig.h"

#include "iotmer_internal.h"

#define TAG "iotmer_wifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     10
#define WIFI_CONNECT_TIMEOUT_MS 30000

static EventGroupHandle_t s_event_group;
static int                s_retry_num;
static bool               s_inited;
static bool               s_connected;

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

    if (strlen(CONFIG_IOTMER_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "CONFIG_IOTMER_WIFI_SSID is empty — set it in menuconfig");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_IOTMER_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_IOTMER_WIFI_PASSWORD,
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

    ESP_LOGI(TAG, "Connecting to SSID: %s ...", CONFIG_IOTMER_WIFI_SSID);

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
