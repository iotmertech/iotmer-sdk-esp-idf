#pragma once

/**
 * @file iotmer_ble.h
 *
 * IOTMER BLE JSON channel (NimBLE GATT server).
 *
 * This component provides a small, general-purpose BLE transport for UTF-8 JSON payloads.
 * Applications can implement Wi-Fi provisioning or other device commands on top.
 *
 * Requires: ESP-IDF >= 5.0, NimBLE enabled, `CONFIG_IOTMER_BLE=y`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Canonical 128-bit UUID strings (RFC-4122 form) for mobile implementations. */
#define IOTMER_BLE_UUID_SVC_STR "1d14d6ee-1001-4000-8024-b5a3c0ffee01"
#define IOTMER_BLE_UUID_RX_STR  "1d14d6ee-1002-4000-8024-b5a3c0ffee01"
#define IOTMER_BLE_UUID_TX_STR  "1d14d6ee-1003-4000-8024-b5a3c0ffee01"

typedef struct iotmer_ble_cfg {
    void *user_ctx;

    /**
     * Optional; invoked from the NimBLE host context.
     * Keep it fast: parse, validate, then hand off to another task if needed.
     *
     * @param user_ctx User context from this config.
     * @param data JSON bytes (not NUL-terminated).
     * @param len Length in bytes.
     */
    void (*on_rx_json)(void *user_ctx, const uint8_t *data, size_t len);
} iotmer_ble_cfg_t;

#define IOTMER_BLE_CFG_DEFAULT() \
    (iotmer_ble_cfg_t) {         \
        .user_ctx    = NULL,     \
        .on_rx_json  = NULL,     \
    }

/**
 * Initialise NimBLE host, GAP/GATT services, and the IOTMER BLE service.
 * Call once after your application initialises NVS (`nvs_flash_init()`).
 * Repeated init returns ESP_OK.
 */
esp_err_t iotmer_ble_init(const iotmer_ble_cfg_t *cfg);

/** Start connectable advertising (name in scan response, UUID in advertising payload). */
esp_err_t iotmer_ble_start(void);

/** Stop advertising and disconnect any active connection. */
esp_err_t iotmer_ble_stop(void);

/** Deinitialise NimBLE host. Call `iotmer_ble_stop()` first. */
void iotmer_ble_deinit(void);

/**
 * Send a JSON payload to the connected central via TX notify (and updates the readable TX value).
 * Returns `ESP_ERR_INVALID_STATE` if not connected or notifications aren't enabled.
 */
esp_err_t iotmer_ble_send_json(const uint8_t *data, size_t len);

/** Convenience wrapper for NUL-terminated strings. */
esp_err_t iotmer_ble_send_json_str(const char *json_str);

/** True when a BLE central is currently connected. */
bool iotmer_ble_is_connected(void);

#ifdef __cplusplus
}
#endif

