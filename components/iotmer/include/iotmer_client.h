#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*iotmer_message_cb_t)(const char *topic,
                                   const char *payload,
                                   int payload_len,
                                   void *ctx);

typedef struct {
    char device_id[64];
    char device_key[96];

    char firmware_checksum_sha256[65];
    uint32_t firmware_size_bytes;
    /** HTTPS OTA source; keep large enough for signed CDN URLs (was 256 → silent truncate). */
    char firmware_url[512];
    /** Set by SDK after a successful auto-OTA (NVS); used to skip re-flashing the same build. */
    char firmware_applied_sha256[65];

    char mqtt_host[128];
    int  mqtt_port;
    bool mqtt_tls;

    char mqtt_username[96];
    char mqtt_password[96];

    /**
     * Opaque `device_http_token` (`dht_…`) from `POST /provision/device`.
     * Use only for device-auth HTTP routes under `…/devices/auth/` (e.g. bind-claim) with Bearer.
     * Persisted in NVS as key "dht" when set. If NVS dht is empty, @c IOTMER_DEVICE_HTTP_TOKEN
     * (Kconfig) may be used as a dev fallback when loading creds. Not used for MQTT.
     */
    char device_http_token[384];

    /** Workspace slug from provision JSON; MQTT ACL topics: {workspace_slug}/{device_key}/… */
    char workspace_slug[64];
} iotmer_creds_t;

typedef struct {
    int  broker_port;
    bool tls;

    int keepalive_sec;
    int reconnect_delay_ms;

    /**
     * Presence (LWT) feature:
     * - On connect: publish retained "ONLINE" to {workspace_slug}/{device_key}/presence/
     * - On unexpected disconnect: broker publishes retained "OFFLINE" (MQTT last will) to same topic
     */
    bool presence_lwt_enable;
} iotmer_config_t;

#define IOTMER_CONFIG_DEFAULT()         \
    {                                   \
        .broker_port = 8883,            \
        .tls = true,                    \
        .keepalive_sec = 60,            \
        .reconnect_delay_ms = 5000,     \
        .presence_lwt_enable = false,   \
    }

typedef struct iotmer_client_s iotmer_client_t;

struct iotmer_client_s {
    iotmer_config_t cfg;
    iotmer_creds_t  creds;

    void *mqtt; /* esp_mqtt_client_handle_t */

    bool connected;

    iotmer_message_cb_t commands_cb;
    void *              commands_ctx;
    iotmer_message_cb_t config_cb;
    void *              config_ctx;

    void *reconnect_timer; /* esp_timer_handle_t */

    /* Presence/LWT topic buffer (must outlive esp_mqtt_client_config_t). */
    char presence_topic[256];

    /**
     * After CONNACK auth failure (rc 4/5), reconnect uses this delay (ms) instead of
     * reconnect_delay_ms, doubling up to 5 minutes to avoid broker flapping / bans.
     * Reset to 0 on successful MQTT_EVENT_CONNECTED.
     */
    uint32_t mqtt_auth_backoff_ms;
};

esp_err_t iotmer_init(iotmer_client_t *client, const iotmer_config_t *cfg);
esp_err_t iotmer_connect(iotmer_client_t *client);
void      iotmer_disconnect(iotmer_client_t *client);

esp_err_t iotmer_telemetry_publish(iotmer_client_t *client, const char *json);
esp_err_t iotmer_state_publish(iotmer_client_t *client, const char *json);
esp_err_t iotmer_presence_publish(iotmer_client_t *client, const char *status);

esp_err_t iotmer_subscribe_commands(iotmer_client_t *client,
                                   iotmer_message_cb_t cb, void *ctx);
esp_err_t iotmer_subscribe_config(iotmer_client_t *client,
                                 iotmer_message_cb_t cb, void *ctx);

/** MQTT Config Protocol v1 (meta / get / resp / status): see `iotmer_config.h`. */

/**
 * Load persisted IOTMER credentials from NVS (namespace from Kconfig `IOTMER_NVS_NAMESPACE`).
 */
esp_err_t iotmer_nvs_load_creds(iotmer_creds_t *out);

/**
 * Store credentials in NVS (replaces same keys; includes `device_http_token` when non-empty).
 */
esp_err_t iotmer_nvs_save_creds(const iotmer_creds_t *creds);

/**
 * HTTPS device provisioning: `POST {IOTMER_PROVISION_API_URL}/provision/device` with
 * `iotmer-auth-code`. Fills @p creds (MQTT, `device_http_token` / NVS `dht`, firmware metadata,
 * etc.) from the JSON body (root or nested `data` where supported). Current API responses
 * typically include `device_http_token` (`dht_…`); it is required for
 * `iotmer_device_auth_bind_claim()`.
 * When `IOTMER_PROVISION_AUTH_CODE` is empty and NVS already has a full session, returns ESP_OK
 * without HTTP (see `https_performed`). See public provisioning docs.
 */
esp_err_t iotmer_provision(iotmer_creds_t *creds, bool *https_performed);

/**
 * `POST {base}/devices/auth/bind-claim` with `Authorization: Bearer` @p creds->device_http_token`
 * and JSON body `{"claim_code": ... }`. @p claim_code is typically from BLE (see `iotmer_ble_wifi_prov`).
 * **Do not** log @p creds or @p claim_code; clear sensitive buffers after use in product code.
 */
esp_err_t iotmer_device_auth_bind_claim(const iotmer_creds_t *creds, const char *claim_code);

#ifdef __cplusplus
}
#endif

