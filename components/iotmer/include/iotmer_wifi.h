#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Store WiFi credentials in NVS (namespace: CONFIG_IOTMER_NVS_NAMESPACE).
 *
 * Intended for dynamic provisioning (e.g. BLE / captive portal). After storing,
 * call `iotmer_wifi_reconnect()` or reboot to apply.
 */
esp_err_t iotmer_wifi_set_credentials(const char *ssid, const char *password);

/**
 * Read WiFi credentials from NVS.
 *
 * @return ESP_OK when both ssid and password are present; ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t iotmer_wifi_get_credentials(char *ssid_out, size_t ssid_out_len,
                                      char *password_out, size_t password_out_len);

/** Erase stored WiFi credentials from NVS (best-effort). */
esp_err_t iotmer_wifi_clear_credentials(void);

/**
 * Disconnect (if needed) and reconnect using the latest credentials.
 * Uses NVS first, then falls back to Kconfig values.
 */
esp_err_t iotmer_wifi_reconnect(void);

/**
 * Non-blocking STA restart. Returns as soon as `esp_wifi_start()` has been
 * requested — it does not wait for an IP and does not call `iotmer_wifi_connect()`
 * (that path blocks up to 30 s).
 *
 * Drops the BSSID lock so the next join scans all channels and picks the
 * strongest AP (authmode threshold, PMF, and failure retry count are unchanged).
 * Clears connection/retry/backoff state and stops the backoff timer, then
 * `esp_wifi_disconnect()`, `esp_wifi_stop()`, `esp_wifi_start()`.
 * `WIFI_EVENT_STA_START` performs the join when autoconnect is enabled; this
 * function does not call `esp_wifi_connect()`.
 *
 * @return ESP_OK once start has been requested.
 *         ESP_ERR_INVALID_STATE if reconnect hold is set (radio is not touched;
 *         release the hold first) or if no STA credential exists (NVS, else Kconfig).
 *         ESP_ERR_WIFI_NOT_STARTED from disconnect/stop is ignored. Any other
 *         error from `esp_wifi_stop()` / `esp_wifi_start()` is returned.
 */
esp_err_t iotmer_wifi_kick(void);

/**
 * Hold WiFi reconnect (fast retry + backoff) while BLE operational session is active.
 * Prevents STA coexist storms from fighting BLE notifies / relay confirm.
 * On release, schedules backoff reconnect if still without IP.
 */
void iotmer_wifi_set_reconnect_hold(bool hold);

/**
 * Gate automatic STA joins. Default is enabled: `WIFI_EVENT_STA_START` calls
 * `esp_wifi_connect()`, and disconnect/backoff keep retrying.
 *
 * When disabled, `STA_START` only records that the radio is up and does not
 * connect; disconnect and backoff do not reconnect until this is enabled again.
 * Credentials are not cleared. Enabling autoconnect does not itself connect.
 *
 * Use this before `esp_wifi_start()` for a provisioning scan while the radio
 * was stopped (for example for BLE), so the saved AP is not joined and the
 * scan is not aborted.
 */
void iotmer_wifi_set_autoconnect(bool enable);

#ifdef __cplusplus
}
#endif

