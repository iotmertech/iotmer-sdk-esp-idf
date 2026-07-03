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

/** Forward declaration so the config callbacks below can reference the client. */
typedef struct iotmer_client_s iotmer_client_t;

/** Max number of concurrently registered topic subscriptions (see iotmer_subscribe). */
#ifndef IOTMER_MAX_SUBSCRIPTIONS
#define IOTMER_MAX_SUBSCRIPTIONS 8
#endif

/**
 * Max stored topic filter length (incl. NUL). Must fit {workspace_slug}/{device_key}/config/#
 * where workspace_slug is up to 63 and device_key up to 95 chars.
 */
#ifndef IOTMER_TOPIC_FILTER_MAX
#define IOTMER_TOPIC_FILTER_MAX 192
#endif

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
     * Presence (LWT) feature. Topic: {workspace_slug}/{device_key}/presence (retained).
     * - On connect: publish {"status":"online","ts":<unix s|0>}
     * - On unexpected disconnect: broker delivers the MQTT last will
     *   {"status":"offline","ts":0} to the same topic.
     */
    bool presence_lwt_enable;

    /**
     * Phantom (half-open) connection detection.
     * When > 0: if `connected` is set but no broker activity
     * (CONNECTED / SUBACK / PUBACK / DATA) is seen for this many milliseconds, the SDK
     * runs iotmer_reconnect_hard() and invokes `on_phantom_detected` (if set).
     * 0 disables the watchdog. Typical value: a few keepalive intervals (e.g. 45000).
     */
    uint32_t phantom_timeout_ms;

    /** User context passed to every callback below. */
    void *user_ctx;

    /** Called after MQTT_EVENT_CONNECTED (post-resubscribe). Optional. */
    void (*on_connected)(iotmer_client_t *c, void *user_ctx);
    /** Called after MQTT_EVENT_DISCONNECTED. Optional. */
    void (*on_disconnected)(iotmer_client_t *c, void *user_ctx);
    /** Called on MQTT_EVENT_PUBLISHED (QoS1/2 PUBACK/PUBCOMP). Optional. */
    void (*on_published)(iotmer_client_t *c, int msg_id, void *user_ctx);
    /** Called on MQTT_EVENT_SUBSCRIBED (SUBACK). `rc` is the granted QoS or negative on failure. */
    void (*on_subscribed)(iotmer_client_t *c, int msg_id, int rc, void *user_ctx);
    /** Called on CONNACK auth failure (rc 4/5); a good place to trigger HTTPS re-provision. */
    void (*on_auth_rejected)(iotmer_client_t *c, int connack_rc, void *user_ctx);
    /** Called just before the SDK hard-reconnects due to a phantom connection. Optional. */
    void (*on_phantom_detected)(iotmer_client_t *c, void *user_ctx);

    /**
     * TLS coexistence hooks (A1). Optional.
     * `on_tls_acquire` is invoked right before the SDK needs contiguous internal RAM for a
     * TLS handshake (initial connect / reconnect). Applications may free memory here
     * (e.g. iotmer_ble_suspend()). `on_tls_release` is invoked once the connection is
     * established or the attempt failed, so the app can restore released resources.
     */
    void (*on_tls_acquire)(void *user_ctx);
    void (*on_tls_release)(void *user_ctx);
} iotmer_config_t;

#define IOTMER_CONFIG_DEFAULT()         \
    {                                   \
        .broker_port = 8883,            \
        .tls = true,                    \
        .keepalive_sec = 60,            \
        .reconnect_delay_ms = 5000,     \
        .presence_lwt_enable = false,   \
        .phantom_timeout_ms = 0,        \
    }

/** One registered subscription (see iotmer_subscribe). Internal use. */
typedef struct {
    char                filter[IOTMER_TOPIC_FILTER_MAX];
    int                 qos;
    iotmer_message_cb_t cb;
    void               *ctx;
    bool                in_use;
} iotmer_subscription_t;

struct iotmer_client_s {
    iotmer_config_t cfg;
    iotmer_creds_t  creds;

    void *mqtt; /* esp_mqtt_client_handle_t */

    bool connected;

    /** Registered subscriptions; re-issued on every MQTT_EVENT_CONNECTED. */
    iotmer_subscription_t subs[IOTMER_MAX_SUBSCRIPTIONS];

    void *reconnect_timer; /* esp_timer_handle_t (one-shot) */
    void *phantom_timer;   /* esp_timer_handle_t (periodic; only when phantom_timeout_ms>0) */

    /**
     * Timestamp (esp_timer_get_time) of the last inbound broker activity
     * (CONNECTED / SUBACK / PUBACK / DATA). 0 means "no activity yet".
     */
    volatile int64_t last_broker_activity_us;

    /* Presence/LWT topic buffer (must outlive esp_mqtt_client_config_t). */
    char presence_topic[256];

    /**
     * Reassembly state for MQTT messages larger than the esp-mqtt RX buffer
     * (delivered as multiple MQTT_EVENT_DATA fragments). Internal use.
     * `rx_assembly` is heap-allocated on demand and capped by
     * CONFIG_IOTMER_MQTT_RX_ASSEMBLY_MAX (0 disables reassembly: oversized
     * messages are dropped with a warning instead of being dispatched broken).
     */
    char  *rx_assembly;
    int    rx_assembly_total;    /* total_data_len of the message in flight */
    int    rx_assembly_received; /* bytes accumulated so far */
    bool   rx_assembly_dropping; /* true: discard fragments until message ends */
    char   rx_assembly_topic[256];

    /**
     * After CONNACK auth failure (rc 4/5), reconnect uses this delay (ms) instead of
     * reconnect_delay_ms, doubling up to 5 minutes to avoid broker flapping / bans.
     * Reset to 0 on successful MQTT_EVENT_CONNECTED.
     */
    uint32_t mqtt_auth_backoff_ms;
};

/**
 * All-in-one bring-up: WiFi -> NVS load -> provision (skipped when already claimed) ->
 * NVS save -> OTA. MQTT is started separately via iotmer_connect().
 *
 * "Claimed / already-provisioned" fast path is the default: when the compiled auth code
 * is empty and NVS already holds a full session, the HTTPS `/provision/device` round-trip
 * is skipped (no credential churn on every boot).
 *
 * For finer control, compose the individual steps yourself (see the iotmer_wifi_up /
 * iotmer_provision / iotmer_nvs_* / iotmer_ota_apply_if_needed / iotmer_connect below).
 */
esp_err_t iotmer_init(iotmer_client_t *client, const iotmer_config_t *cfg);
esp_err_t iotmer_connect(iotmer_client_t *client);
void      iotmer_disconnect(iotmer_client_t *client);

/**
 * Full MQTT teardown + fresh start (stop + destroy + init + start), preserving the
 * client config, registered subscriptions and callbacks. Use this when the socket is
 * dead / half-open and esp_mqtt_client_reconnect() no longer recovers. Triggered
 * automatically by the phantom watchdog and by the reconnect timer as a fallback.
 */
esp_err_t iotmer_reconnect_hard(iotmer_client_t *client);

/**
 * Milliseconds since the last inbound broker activity (CONNECTED / SUBACK / PUBACK / DATA).
 * Returns -1 when no activity has been observed yet.
 */
int64_t iotmer_ms_since_broker_activity(const iotmer_client_t *client);

/* --- Composable bring-up steps (iotmer_init is a convenience wrapper over these) --- */

/**
 * Connect WiFi (blocks the calling task for up to ~30 s until IP or timeout).
 * Uses NVS creds first, then Kconfig.
 *
 * Even when this call returns ESP_FAIL / ESP_ERR_TIMEOUT, the SDK keeps retrying
 * the link in the background with backoff (15–60 s) until an IP is acquired —
 * a router outage never permanently strands the device.
 *
 * Stack note: when calling from your own task (directly or via iotmer_init(),
 * which also runs HTTPS provisioning/TLS), give that task at least 8 KB of stack.
 */
esp_err_t iotmer_wifi_up(void);

/**
 * HTTPS OTA if the provisioned firmware SHA differs from the last applied image.
 * @param after_https_provision  Force OTA even when SHA matches (see IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA).
 */
esp_err_t iotmer_ota_apply_if_needed(iotmer_creds_t *creds, bool after_https_provision);

/**
 * Finalize a provisioning/claim flow. Cleanly disconnects MQTT (if @p client is non-NULL
 * and connected), then optionally reboots after @p reboot_delay_ms to enter a clean
 * "online" boot without BLE/MQTT/HTTPS contending in the same session.
 */
void iotmer_finalize_provisioning(iotmer_client_t *client, bool reboot, uint32_t reboot_delay_ms);

esp_err_t iotmer_telemetry_publish(iotmer_client_t *client, const char *json);
esp_err_t iotmer_state_publish(iotmer_client_t *client, const char *json);

/**
 * Publish retained presence JSON {"status":"<status>","ts":<unix s|0>} to
 * {workspace_slug}/{device_key}/presence. @p status is lowercased (e.g. "online").
 * Called automatically on connect when presence_lwt_enable is set.
 */
esp_err_t iotmer_presence_publish(iotmer_client_t *client, const char *status);

/**
 * Register (or update) a subscription. The topic filter may contain MQTT wildcards
 * (`+`, `#`). The SDK stores it and re-issues it on every (re)connect, and dispatches
 * matching inbound messages to @p cb. Re-registering the same filter updates cb/ctx/qos.
 */
esp_err_t iotmer_subscribe(iotmer_client_t *client, const char *topic_filter, int qos,
                           iotmer_message_cb_t cb, void *ctx);

/** Remove a previously registered subscription (unsubscribes at the broker if connected). */
esp_err_t iotmer_unsubscribe(iotmer_client_t *client, const char *topic_filter);

/** Convenience: subscribe to the console command topic {workspace_slug}/{device_key}/cmd (QoS1). */
esp_err_t iotmer_subscribe_commands(iotmer_client_t *client,
                                   iotmer_message_cb_t cb, void *ctx);
/** Convenience: subscribe to {workspace_slug}/{device_key}/config/# (QoS1). */
esp_err_t iotmer_subscribe_config(iotmer_client_t *client,
                                 iotmer_message_cb_t cb, void *ctx);

/** MQTT Config Protocol (meta / get / resp / status): see `iotmer_config.h`. */

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
 * Input: if `creds->device_key` is set before the call, it is sent as `device_key` in the POST
 * body (required by the console API on first registration).
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

