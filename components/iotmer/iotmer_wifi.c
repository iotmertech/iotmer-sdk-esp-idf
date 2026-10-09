/*
 * iotmer_wifi.c — WiFi STA helper.
 *
 * iotmer_wifi_connect() is idempotent: calling it a second time after a
 * successful connection is a no-op (returns ESP_OK immediately).
 *
 * Internally it uses an event group to block until an IP is acquired or
 * the connection attempt fails. The fast-retry limit (WIFI_FAST_RETRY_MAX)
 * only bounds how long the *blocking* call waits before returning ESP_FAIL:
 * the link itself is never abandoned. Once fast retries are exhausted, a
 * one-shot esp_timer keeps calling esp_wifi_connect() with an increasing
 * backoff (WIFI_BACKOFF_MIN_MS … WIFI_BACKOFF_MAX_MS) until an IP is
 * re-acquired, so a router that comes back hours later is still picked up.
 *
 * Multi-AP (same SSID): connect scans all channels and picks the strongest AP;
 * after IP is acquired the BSSID is locked to avoid mid-session roaming drops.
 * On disconnect, BSSID lock is cleared and reconnect may stop/start the radio
 * (same effect as a power cycle) when roaming or repeated failures leave the
 * driver stuck.
 *
 * iotmer_wifi_kick() is the non-blocking form of that restart: it drops the
 * BSSID lock, cancels backoff, and stop/starts the radio, then returns.
 * It does not call iotmer_wifi_connect() and does not wait for an IP.
 * WIFI_EVENT_STA_START joins only while autoconnect is enabled (the default).
 *
 * Note: nvs_flash_init() and esp_event_loop_create_default() are the
 * application's responsibility; this module calls them defensively and
 * tolerates ESP_ERR_INVALID_STATE (already initialised).
 */

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "iotmer_internal.h"
#include "iotmer_wifi.h"

#define TAG "iotmer_wifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_FAST_RETRY_MAX 10
#define WIFI_CONNECT_TIMEOUT_MS 30000

/* Persistent reconnect backoff once fast retries are exhausted. */
#define WIFI_BACKOFF_MIN_MS 15000
#define WIFI_BACKOFF_MAX_MS 60000

/* Multi-AP (same SSID): try several APs before giving up on one scan pass. */
#define WIFI_STA_FAILURE_RETRY_CNT 5

/* After this many fast disconnect retries, stop/start the radio (power-cycle equivalent). */
#define WIFI_FULL_RESTART_FAST_RETRY 3

/* Keep keys ≤15 chars (ESP-IDF NVS limit). */
#define IOTMER_WIFI_NVS_KEY_SSID "wifi_ssid"
#define IOTMER_WIFI_NVS_KEY_PASS "wifi_pass"

static EventGroupHandle_t  s_event_group;
static int                 s_retry_num;
static bool                s_inited;
static bool                s_started;   /* esp_wifi_start() done (STA_START seen) */
static bool                s_connected; /* IP acquired; cleared on STA_DISCONNECTED */
static esp_timer_handle_t  s_backoff_timer;
static uint32_t            s_backoff_ms;
static bool                s_reconnect_hold;
/* STA_START / disconnect / backoff join only while this is true. Default on. */
static bool                s_autoconnect = true;
static char                s_sta_ssid[sizeof(((wifi_config_t *)0)->sta.ssid)];
static char                s_sta_pass[sizeof(((wifi_config_t *)0)->sta.password)];

static esp_err_t wifi_creds_load_from_nvs(char *ssid_out,
                                          size_t ssid_out_len,
                                          char *pass_out,
                                          size_t pass_out_len);
static void wifi_creds_save_to_nvs_if_possible(const char *ssid, const char *pass);

static void wifi_fill_sta_config(wifi_config_t *wifi_cfg,
                                 const char *ssid,
                                 const char *pass,
                                 bool lock_bssid,
                                 const uint8_t *bssid)
{
    memset(wifi_cfg, 0, sizeof(*wifi_cfg));
    strncpy((char *)wifi_cfg->sta.ssid, ssid, sizeof(wifi_cfg->sta.ssid) - 1);
    strncpy((char *)wifi_cfg->sta.password, pass, sizeof(wifi_cfg->sta.password) - 1);
    /* Empty password = open network; a WPA2 floor there would never associate. */
    wifi_cfg->sta.threshold.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN
                                                         : WIFI_AUTH_WPA2_PSK;
    wifi_cfg->sta.pmf_cfg.capable    = true;
    wifi_cfg->sta.pmf_cfg.required   = false;
    /* Prefer the strongest AP when several share the same SSID. */
    wifi_cfg->sta.scan_method        = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg->sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg->sta.failure_retry_cnt  = WIFI_STA_FAILURE_RETRY_CNT;
    if (lock_bssid && bssid != NULL) {
        wifi_cfg->sta.bssid_set = true;
        memcpy(wifi_cfg->sta.bssid, bssid, sizeof(wifi_cfg->sta.bssid));
    }
}

static esp_err_t wifi_load_sta_credentials(char *ssid, size_t ssid_len,
                                           char *pass, size_t pass_len)
{
    if (wifi_creds_load_from_nvs(ssid, ssid_len, pass, pass_len) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials loaded from NVS (ns=%s)", CONFIG_IOTMER_NVS_NAMESPACE);
        return ESP_OK;
    }

    if (strlen(CONFIG_IOTMER_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "CONFIG_IOTMER_WIFI_SSID is empty — set it in menuconfig (or store WiFi in NVS)");
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(ssid, CONFIG_IOTMER_WIFI_SSID, ssid_len - 1);
    ssid[ssid_len - 1] = '\0';
    strncpy(pass, CONFIG_IOTMER_WIFI_PASSWORD, pass_len - 1);
    pass[pass_len - 1] = '\0';
    wifi_creds_save_to_nvs_if_possible(ssid, pass);
    return ESP_OK;
}

static esp_err_t wifi_apply_sta_config(bool lock_bssid, const uint8_t *bssid)
{
    if (s_sta_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t wifi_cfg;
    wifi_fill_sta_config(&wifi_cfg, s_sta_ssid, s_sta_pass, lock_bssid, bssid);
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

static bool wifi_disconnect_needs_full_restart(int reason)
{
    switch (reason) {
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:
    case WIFI_REASON_ROAMING:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return true;
    default:
        return false;
    }
}

static void wifi_lock_bssid_on_got_ip(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return;
    }

    esp_err_t err = wifi_apply_sta_config(true, ap_info.bssid);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Locked to AP bssid=" MACSTR " rssi=%d ch=%u",
                 MAC2STR(ap_info.bssid), ap_info.rssi, (unsigned)ap_info.primary);
    } else {
        ESP_LOGW(TAG, "BSSID lock failed: %s", esp_err_to_name(err));
    }
}

static void wifi_sta_reconnect_attempt(bool full_restart)
{
    if (s_reconnect_hold || !s_autoconnect) {
        return;
    }

    if (s_sta_ssid[0] == '\0') {
        esp_err_t err = wifi_load_sta_credentials(s_sta_ssid, sizeof(s_sta_ssid),
                                                  s_sta_pass, sizeof(s_sta_pass));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WiFi reconnect skipped — no credentials");
            return;
        }
    }

    if (full_restart && s_started) {
        ESP_LOGW(TAG, "WiFi full restart (stop/start) before reconnect");
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        s_started = false;
    }

    /* Drop BSSID lock so the next scan can pick the strongest AP again. */
    esp_err_t err = wifi_apply_sta_config(false, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi reconnect config failed: %s", esp_err_to_name(err));
    }

    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_start (reconnect): %s", esp_err_to_name(err));
        }
    } else {
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect (reconnect): %s", esp_err_to_name(err));
        }
    }
}

static void wifi_backoff_timer_cb(void *arg)
{
    (void)arg;
    if (s_reconnect_hold) {
        ESP_LOGI(TAG, "WiFi backoff skip — reconnect held");
        return;
    }
    if (!s_autoconnect) {
        ESP_LOGI(TAG, "WiFi backoff skip — autoconnect off");
        return;
    }
    ESP_LOGI(TAG, "WiFi backoff retry — attempting reconnect");
    wifi_sta_reconnect_attempt(true);
}

/*
 * Never give up on the link: arm a one-shot timer that pokes esp_wifi_connect()
 * again. The delay doubles from WIFI_BACKOFF_MIN_MS up to WIFI_BACKOFF_MAX_MS
 * so a long outage does not hammer the radio, and is reset on GOT_IP.
 */
static void schedule_backoff_reconnect(void)
{
    if (!s_backoff_timer) {
        return;
    }
    if (s_reconnect_hold) {
        ESP_LOGW(TAG, "WiFi backoff deferred — reconnect held");
        return;
    }
    if (!s_autoconnect) {
        ESP_LOGW(TAG, "WiFi backoff deferred — autoconnect off");
        return;
    }
    if (s_backoff_ms == 0U) {
        s_backoff_ms = WIFI_BACKOFF_MIN_MS;
    } else if (s_backoff_ms < WIFI_BACKOFF_MAX_MS) {
        s_backoff_ms *= 2U;
        if (s_backoff_ms > WIFI_BACKOFF_MAX_MS) {
            s_backoff_ms = WIFI_BACKOFF_MAX_MS;
        }
    }
    (void)esp_timer_stop(s_backoff_timer);
    if (esp_timer_start_once(s_backoff_timer, (uint64_t)s_backoff_ms * 1000ULL) == ESP_OK) {
        ESP_LOGW(TAG, "WiFi still down — next reconnect attempt in %" PRIu32 " ms",
                 s_backoff_ms);
    }
}

void iotmer_wifi_set_reconnect_hold(bool hold)
{
    if (s_reconnect_hold == hold) {
        return;
    }
    s_reconnect_hold = hold;
    if (hold) {
        if (s_backoff_timer) {
            (void)esp_timer_stop(s_backoff_timer);
        }
        ESP_LOGI(TAG, "WiFi reconnect HOLD");
        return;
    }
    ESP_LOGI(TAG, "WiFi reconnect RELEASE");
    if (!s_connected) {
        schedule_backoff_reconnect();
    }
}

void iotmer_wifi_set_autoconnect(bool enable)
{
    if (s_autoconnect == enable) {
        return;
    }
    s_autoconnect = enable;
    if (!enable) {
        /* A 15 s backoff already armed must not join over a provisioning scan. */
        if (s_backoff_timer) {
            (void)esp_timer_stop(s_backoff_timer);
        }
        ESP_LOGI(TAG, "WiFi autoconnect OFF");
        return;
    }
    ESP_LOGI(TAG, "WiFi autoconnect ON");
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
    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
        /* Don't wait for the async STA_STOP event; the stop already happened. */
        s_started = false;
    }

    return iotmer_wifi_connect();
}

esp_err_t iotmer_wifi_kick(void)
{
    /* Hold owns the radio. Caller releases it before a restart. */
    if (s_reconnect_hold) {
        return ESP_ERR_INVALID_STATE;
    }

    /* NVS, else Kconfig. Do not touch the radio when neither has an SSID.
     * Load into locals so a miss does not wipe a previous RAM copy. */
    char ssid[sizeof(s_sta_ssid)];
    char pass[sizeof(s_sta_pass)];
    ssid[0] = '\0';
    pass[0] = '\0';
    esp_err_t err = wifi_load_sta_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK || ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    strncpy(s_sta_pass, pass, sizeof(s_sta_pass) - 1);
    s_sta_pass[sizeof(s_sta_pass) - 1] = '\0';

    err = wifi_init_once();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    /* Drop BSSID lock. Next join uses WIFI_ALL_CHANNEL_SCAN and
     * WIFI_CONNECT_AP_BY_SIGNAL; authmode, PMF, and failure_retry_cnt stay. */
    err = wifi_apply_sta_config(false, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_connected = false;
    s_retry_num = 0;
    s_backoff_ms = 0;
    if (s_backoff_timer) {
        (void)esp_timer_stop(s_backoff_timer);
    }

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        /* ignore — not associated is fine; stop/start still restarts the STA */
    }
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return err;
    }
    /* WIFI_EVENT_STA_STOP is async; don't wait for it to clear s_started. */
    s_started = false;

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_start (kick): %s", esp_err_to_name(err));
        return err;
    }

    /* STA_START calls esp_wifi_connect() when autoconnect is on. Do not
     * connect again here, and do not wait on the event group. */
    ESP_LOGI(TAG, "WiFi kick — start requested (ssid=%s)", s_sta_ssid);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (s_autoconnect) {
            (void)esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "WiFi STA started — autoconnect off");
        }

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        s_started = false;
        s_connected = false;
        if (s_backoff_timer) {
            (void)esp_timer_stop(s_backoff_timer);
        }

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)event_data;
        const int reason = disc ? (int)disc->reason : -1;

        s_connected = false;

        if (disc) {
            ESP_LOGW(TAG,
                     "WiFi disconnected — reason=%d rssi=%d bssid=" MACSTR
                     "%s",
                     reason, disc->rssi, MAC2STR(disc->bssid),
                     s_reconnect_hold ? " (reconnect held)" :
                     (!s_autoconnect ? " (autoconnect off)" : ""));
        } else {
            ESP_LOGW(TAG, "WiFi disconnected — reason unknown%s",
                     s_reconnect_hold ? " (reconnect held)" :
                     (!s_autoconnect ? " (autoconnect off)" : ""));
        }

        if (s_reconnect_hold) {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
        } else if (!s_autoconnect) {
            /* Provisioning scan owns the radio; do not join the saved AP. */
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
        } else if (s_retry_num < WIFI_FAST_RETRY_MAX) {
            s_retry_num++;
            const bool full_restart =
                (s_retry_num >= WIFI_FULL_RESTART_FAST_RETRY) ||
                wifi_disconnect_needs_full_restart(reason);
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d%s",
                     s_retry_num, WIFI_FAST_RETRY_MAX,
                     full_restart ? " (full restart)" : "");
            wifi_sta_reconnect_attempt(full_restart);
        } else {
            /* Unblock any pending iotmer_wifi_connect() caller, but do NOT give
             * up on the link: keep retrying forever with backoff. */
            ESP_LOGE(TAG, "WiFi connect failed after %d fast retries — switching to backoff",
                     WIFI_FAST_RETRY_MAX);
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            schedule_backoff_reconnect();
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_backoff_ms = 0;
        if (s_backoff_timer) {
            (void)esp_timer_stop(s_backoff_timer);
        }
        s_connected = true;
        wifi_lock_bssid_on_got_ip();
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

    const esp_timer_create_args_t backoff_args = {
        .callback              = &wifi_backoff_timer_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "iotmer_wifi_bo",
        .skip_unhandled_events = true,
    };
    esp_err_t terr = esp_timer_create(&backoff_args, &s_backoff_timer);
    if (terr != ESP_OK) {
        /* Non-fatal: without the timer we lose only the persistent backoff. */
        ESP_LOGW(TAG, "backoff timer create failed: %s", esp_err_to_name(terr));
        s_backoff_timer = NULL;
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

    s_sta_ssid[0] = '\0';
    s_sta_pass[0] = '\0';

    err = wifi_load_sta_credentials(s_sta_ssid, sizeof(s_sta_ssid),
                                              s_sta_pass, sizeof(s_sta_pass));
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    err = wifi_apply_sta_config(false, NULL);
    if (err != ESP_OK) return err;

    /* Clear stale bits before starting (handles repeated calls after failure). */
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_backoff_ms = 0;
    if (s_backoff_timer) {
        (void)esp_timer_stop(s_backoff_timer);
    }

    const bool was_started = s_started;

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * If WiFi was already started, esp_wifi_start() emits no STA_START event and
     * the STA_START → esp_wifi_connect() chain never fires (that chain runs only
     * while autoconnect is enabled). Kick the connection directly so the call
     * below doesn't just sit out the 30 s timeout.
     */
    if (was_started) {
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect (already-started path): %s",
                     esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s (all-channel, strongest AP) ...", s_sta_ssid);

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
