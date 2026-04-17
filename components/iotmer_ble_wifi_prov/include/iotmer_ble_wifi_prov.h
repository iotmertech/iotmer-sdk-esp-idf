#pragma once

/**
 * @file iotmer_ble_wifi_prov.h
 *
 * IOTMER-defined BLE GATT Wi-Fi credential provisioning (protocol v1).
 *
 * This module is orchestration-agnostic: the application chooses when to call
 * @ref iotmer_ble_wifi_prov_start / @ref iotmer_ble_wifi_prov_stop relative to Wi-Fi,
 * MQTT, HTTPS provisioning, etc. Wi-Fi credentials are persisted only via
 * @ref iotmer_wifi_set_credentials (see @ref iotmer_wifi.h).
 *
 * Requires Kconfig @c CONFIG_IOTMER_BLE_WIFI_PROV and NimBLE enabled in sdkconfig.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version carried in event notifications (byte 0 of payload). */
#define IOTMER_BLE_WIFI_PROV_PROTO_VER 1u

/**
 * Canonical 128-bit UUID strings (RFC-4122 form) for mobile implementation.
 * Wire order on-air follows BLE UUID128 byte order; see documentation.
 */
#define IOTMER_BLE_WIFI_PROV_UUID_SVC_STR  "1d14d6ee-0001-4000-8024-b5a3c0ffee01"
#define IOTMER_BLE_WIFI_PROV_UUID_CTRL_STR "1d14d6ee-0101-4000-8024-b5a3c0ffee01"
#define IOTMER_BLE_WIFI_PROV_UUID_DATA_STR "1d14d6ee-0202-4000-8024-b5a3c0ffee01"
#define IOTMER_BLE_WIFI_PROV_UUID_EVT_STR  "1d14d6ee-0303-4000-8024-b5a3c0ffee01"

/** IEEE 802.11 SSID octet limit (UTF-8). */
#define IOTMER_BLE_WIFI_PROV_SSID_MAX 32u
/** ESP-IDF @c wifi_sta_config_t password field size minus NUL. */
#define IOTMER_BLE_WIFI_PROV_PASS_MAX 63u

/** First byte of Control characteristic writes (v1). */
typedef enum {
    IOTMER_BLE_WIFI_PROV_OP_PING       = 0x01u,
    IOTMER_BLE_WIFI_PROV_OP_BEGIN_SSID = 0x02u,
    IOTMER_BLE_WIFI_PROV_OP_BEGIN_PASS = 0x03u,
    IOTMER_BLE_WIFI_PROV_OP_COMMIT     = 0x04u,
    IOTMER_BLE_WIFI_PROV_OP_ABORT      = 0x05u,
} iotmer_ble_wifi_prov_opcode_t;

/** Byte 1 of event notification (after proto @ref IOTMER_BLE_WIFI_PROV_PROTO_VER). */
typedef enum {
    IOTMER_BLE_WIFI_PROV_EVT_STATE    = 0u,
    IOTMER_BLE_WIFI_PROV_EVT_PROGRESS = 1u,
    IOTMER_BLE_WIFI_PROV_EVT_DONE     = 2u,
    IOTMER_BLE_WIFI_PROV_EVT_ERROR    = 3u,
} iotmer_ble_wifi_prov_evt_code_t;

/** Application-visible error codes (bytes 2–3 LE in event frame). */
typedef enum {
    IOTMER_BLE_WIFI_PROV_ERR_NONE            = 0u,
    IOTMER_BLE_WIFI_PROV_ERR_INVALID_STATE   = 1u,
    IOTMER_BLE_WIFI_PROV_ERR_SSID_TOO_LONG   = 2u,
    IOTMER_BLE_WIFI_PROV_ERR_PASS_TOO_LONG   = 3u,
    IOTMER_BLE_WIFI_PROV_ERR_SSID_EMPTY      = 4u,
    IOTMER_BLE_WIFI_PROV_ERR_PASS_EMPTY      = 5u,
    IOTMER_BLE_WIFI_PROV_ERR_BAD_OPCODE      = 6u,
    IOTMER_BLE_WIFI_PROV_ERR_SESSION_TIMEOUT = 7u,
    IOTMER_BLE_WIFI_PROV_ERR_ADV_TIMEOUT     = 8u,
    IOTMER_BLE_WIFI_PROV_ERR_NVS             = 9u,
    IOTMER_BLE_WIFI_PROV_ERR_INTERNAL        = 10u,
    IOTMER_BLE_WIFI_PROV_ERR_DISCONNECTED    = 11u,
} iotmer_ble_wifi_prov_err_t;

/** High-level session state for @ref iotmer_ble_wifi_prov_cfg_t.on_state. */
typedef enum {
    IOTMER_BLE_WIFI_PROV_STATE_IDLE = 0,
    IOTMER_BLE_WIFI_PROV_STATE_ADVERTISING,
    IOTMER_BLE_WIFI_PROV_STATE_CONNECTED,
    IOTMER_BLE_WIFI_PROV_STATE_RECEIVING,
    IOTMER_BLE_WIFI_PROV_STATE_COMMITTING,
    IOTMER_BLE_WIFI_PROV_STATE_SUCCESS,
    IOTMER_BLE_WIFI_PROV_STATE_FAILED,
} iotmer_ble_wifi_prov_state_t;

typedef struct iotmer_ble_wifi_prov_cfg {
    void *user_ctx;

    /** Optional; invoked from internal worker task. */
    void (*on_state)(void *user_ctx, iotmer_ble_wifi_prov_state_t state);

    /**
     * Optional; invoked after COMMIT attempts.
     * @param esp_err Result of @ref iotmer_wifi_set_credentials (ESP_OK on success).
     * @param detail Structured BLE protocol error (IOTMER_BLE_WIFI_PROV_ERR_NONE on success).
     */
    void (*on_result)(void *user_ctx, esp_err_t esp_err, iotmer_ble_wifi_prov_err_t detail);

    /** 0 = use Kconfig @c CONFIG_IOTMER_BLE_ADV_TIMEOUT_MS */
    uint32_t adv_timeout_ms;
    /** 0 = use Kconfig @c CONFIG_IOTMER_BLE_CONN_IDLE_MS */
    uint32_t conn_idle_ms;
    /** 0 = use Kconfig @c CONFIG_IOTMER_BLE_SESSION_MAX_MS */
    uint32_t session_max_ms;
} iotmer_ble_wifi_prov_cfg_t;

#define IOTMER_BLE_WIFI_PROV_CFG_DEFAULT() \
    (iotmer_ble_wifi_prov_cfg_t) {         \
        .user_ctx        = NULL,          \
        .on_state        = NULL,          \
        .on_result       = NULL,          \
        .adv_timeout_ms  = 0,            \
        .conn_idle_ms    = 0,            \
        .session_max_ms  = 0,            \
    }

/**
 * Initialise NimBLE host, GATT service, store, and internal worker task.
 * Call once after @c nvs_flash_init(). Thread-safe vs repeated init: second call returns ESP_OK.
 * @param cfg May be @c NULL to use @ref IOTMER_BLE_WIFI_PROV_CFG_DEFAULT().
 */
esp_err_t iotmer_ble_wifi_prov_init(const iotmer_ble_wifi_prov_cfg_t *cfg);

/** Tear down BLE host (call @ref iotmer_ble_wifi_prov_stop first). */
void iotmer_ble_wifi_prov_deinit(void);

/**
 * Begin connectable advertising using GAP name prefix from Kconfig + short suffix.
 * Safe to call from any task (uses NimBLE host lock internally).
 */
esp_err_t iotmer_ble_wifi_prov_start(void);

/**
 * Stop advertising, terminate connection if any, cancel timers.
 * Does not call @ref iotmer_ble_wifi_prov_deinit.
 */
esp_err_t iotmer_ble_wifi_prov_stop(void);

#ifdef __cplusplus
}
#endif
