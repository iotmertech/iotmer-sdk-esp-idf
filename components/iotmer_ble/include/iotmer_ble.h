#pragma once

/**
 * @file iotmer_ble.h
 *
 * IOTMER BLE JSON channel (NimBLE GATT server).
 *
 * This component provides a small, general-purpose BLE transport for UTF-8 JSON payloads.
 * Applications can implement Wi-Fi provisioning or other device commands on top.
 *
 * Requires: ESP-IDF >= 6.0, NimBLE enabled, CONFIG_IOTMER_BLE=y.
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
     * Optional advertised device name. When NULL or empty, the SDK builds a
     * MAC-derived name (CONFIG_IOTMER_BLE_GAP_NAME_PREFIX + hex suffix).
     * The string is copied internally at init; it need not outlive the call.
     */
    const char *device_name;

    /**
     * Optional; invoked with a complete received JSON frame.
     *
     * By default this runs in the NimBLE host context — keep it fast: parse, validate,
     * then hand off to another task if needed. If `rx_queue_len` > 0, the SDK instead
     * dispatches this callback from a dedicated worker task, so heavier work is safe.
     *
     * @param user_ctx User context from this config.
     * @param data JSON bytes (not NUL-terminated).
     * @param len Length in bytes.
     */
    void (*on_rx_json)(void *user_ctx, const uint8_t *data, size_t len);

    /** Optional: a BLE central connected. */
    void (*on_connect)(void *user_ctx);
    /** Optional: the central disconnected. @p reason is the NimBLE HCI reason code. */
    void (*on_disconnect)(void *user_ctx, int reason);
    /** Optional: central toggled notifications on the TX characteristic (CCCD). */
    void (*on_subscribe)(void *user_ctx, bool notify_enabled);
    /** Optional: ATT MTU negotiated / updated for the active connection. */
    void (*on_mtu)(void *user_ctx, uint16_t mtu);

    /**
     * Optional queued RX dispatch (B2). When > 0, the SDK creates a FreeRTOS queue of
     * this depth plus a worker task; `on_rx_json` is invoked from that task instead of
     * the NimBLE host context. 0 keeps the direct (host-context) callback.
     */
    uint16_t rx_queue_len;
    /** Worker task stack (bytes) when `rx_queue_len` > 0. 0 => a sensible default (4096). */
    uint16_t rx_task_stack;
} iotmer_ble_cfg_t;

#define IOTMER_BLE_CFG_DEFAULT()  \
    (iotmer_ble_cfg_t) {          \
        .user_ctx      = NULL,    \
        .device_name   = NULL,    \
        .on_rx_json    = NULL,    \
        .on_connect    = NULL,    \
        .on_disconnect = NULL,    \
        .on_subscribe  = NULL,    \
        .on_mtu        = NULL,    \
        .rx_queue_len  = 0,       \
        .rx_task_stack = 0,       \
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
 * Suspend the BLE radio to reclaim controller RAM (e.g. so a TLS handshake can allocate
 * its contiguous internal buffer). Internally: stop + deinit, remembering the stored
 * config and whether advertising was active. Safe to call when already suspended.
 */
esp_err_t iotmer_ble_suspend(void);

/**
 * Resume after iotmer_ble_suspend(): re-init with the stored config and restart
 * advertising if it was active before suspension. Safe to call when not suspended.
 */
esp_err_t iotmer_ble_resume(void);

/** True while the radio is suspended (between suspend and resume). */
bool iotmer_ble_is_suspended(void);

/**
 * Send a JSON payload to the connected central via TX notify (and updates the readable TX value).
 * Returns `ESP_ERR_INVALID_STATE` if not connected or notifications aren't enabled.
 */
esp_err_t iotmer_ble_send_json(const uint8_t *data, size_t len);

/** Convenience wrapper for NUL-terminated strings. */
esp_err_t iotmer_ble_send_json_str(const char *json_str);

/** True when a BLE central is currently connected. */
bool iotmer_ble_is_connected(void);

/**
 * Returns the currently negotiated ATT MTU for the active connection.
 *
 * @return ATT MTU value when connected; otherwise 0.
 */
uint16_t iotmer_ble_get_att_mtu(void);

#ifdef __cplusplus
}
#endif

