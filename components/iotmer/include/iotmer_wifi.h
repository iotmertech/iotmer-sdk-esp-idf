#pragma once

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

#ifdef __cplusplus
}
#endif

