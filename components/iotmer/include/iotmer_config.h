#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "iotmer_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max chunks supported for one `rid` (bitmap-backed reassembly). */
#define IOTMER_CONFIG_MAX_CHUNKS 256

/** Correlation id (UUID string + NUL). */
#define IOTMER_CONFIG_RID_LEN 40

/** Lowercase hex SHA256 (64 chars + NUL). */
#define IOTMER_CONFIG_SHA_HEX_LEN 65

typedef enum {
    IOTMER_CONFIG_EV_META = 1,
    /** Effective config JSON bytes (UTF-8), stable until callback returns. */
    IOTMER_CONFIG_EV_CONFIG_JSON,
    /** Cloud `config/resp` with ok=false. */
    IOTMER_CONFIG_EV_RESP_ERROR,
    /** Protocol / local validation failure (parse, sha, gzip, buffer). */
    IOTMER_CONFIG_EV_FAIL,
} iotmer_config_ev_type_t;

typedef struct {
    iotmer_config_ev_type_t type;
    /** Last request id when relevant (resp / fail during transfer). */
    char rid[IOTMER_CONFIG_RID_LEN];
    union {
        struct {
            uint32_t     version;
            char         sha256_hex[IOTMER_CONFIG_SHA_HEX_LEN];
            size_t       bytes_hint;
        } meta;
        struct {
            uint32_t     version;
            char         sha256_hex[IOTMER_CONFIG_SHA_HEX_LEN];
            const uint8_t *json_utf8;
            size_t       json_len;
        } config;
        struct {
            char code[48];
            char message[160];
            bool retryable;
        } resp_err;
        struct {
            char message[160];
        } fail;
    } u;
} iotmer_config_event_t;

typedef void (*iotmer_config_event_cb_t)(void *user_ctx, const iotmer_config_event_t *ev);

/**
 * MQTT Config Protocol — reassembly / crypto workspace.
 *
 * Buffer layout (single user-supplied region):
 * - Lower half [0 .. cap/2): decoded `data_b64` payload bytes (gzip stream or raw JSON for identity).
 * - Upper half [cap/2 .. cap): per-message JSON parse buffer and final effective JSON for the callback.
 *
 * Requirements:
 * - cap >= 4096 and even.
 * - cap/2 must fit the largest gzip stream you expect.
 * - cap/2 must fit the largest inflated JSON.
 * - Each `config/resp` JSON text should be <= cap/2 - 1 (copied+NUL for cJSON).
 */
typedef struct {
    uint8_t *buf;
    size_t   cap;

    bool     have_valid;
    uint32_t have_version;
    char     have_sha_hex[IOTMER_CONFIG_SHA_HEX_LEN];

    bool     transfer_active;
    char     pending_rid[IOTMER_CONFIG_RID_LEN];

    bool     chunk_meta_init;
    /** Active `config/resp` transfer uses gzip path (else identity raw JSON in lower half). */
    bool     resp_chunk_is_gzip;
    uint32_t resp_version;
    char     resp_sha_hex[IOTMER_CONFIG_SHA_HEX_LEN];
    uint32_t total_chunks;

    uint32_t chunk_bmap[8]; /* 256 bits */
    size_t   gzip_len;
} iotmer_config_ctx_t;

/** Zero ctx; wire buffer pointers. */
void iotmer_config_ctx_init(iotmer_config_ctx_t *ctx, uint8_t *buf, size_t cap);

/** Optional: declare what the device already applied (sent as `have` on pull). */
void iotmer_config_set_have(iotmer_config_ctx_t *ctx, uint32_t version, const char *sha256_hex);
void iotmer_config_clear_have(iotmer_config_ctx_t *ctx);

/**
 * Dispatch `config/#` MQTT payload (call from `iotmer_subscribe_config` callback).
 * Parses `config/meta` and `config/resp`.
 */
esp_err_t iotmer_config_on_mqtt(iotmer_config_ctx_t *ctx,
                               iotmer_client_t *client,
                               const char *topic,
                               const char *payload,
                               int payload_len,
                               iotmer_config_event_cb_t cb,
                               void *user_ctx);

/**
 * Publish `config/get` (pull). Sets internal `pending_rid` / transfer state on success.
 * `rid_out` receives the same id (for status correlation).
 */
esp_err_t iotmer_config_request(iotmer_config_ctx_t *ctx,
                                iotmer_client_t *client,
                                uint32_t chunk_bytes,
                                uint32_t max_total_bytes,
                                char rid_out[IOTMER_CONFIG_RID_LEN]);

/** Publish `config/status` (QoS1, not retained). */
esp_err_t iotmer_config_publish_status(iotmer_client_t *client,
                                       const char *rid,
                                       bool applied,
                                       uint32_t version,
                                       const char *sha256_hex,
                                       const char *err_code,
                                       const char *err_msg);

#ifdef __cplusplus
}
#endif
