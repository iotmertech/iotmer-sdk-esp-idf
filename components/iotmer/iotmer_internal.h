#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "iotmer_client.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/** Apply current trust store (pinned PEM or crt_bundle) to an HTTP client config. */
void iotmer_tls_apply_http_client_config(esp_http_client_config_t *cfg);

/** Apply current trust store to esp_mqtt broker.verification. */
void iotmer_tls_apply_mqtt_verification(esp_mqtt_client_config_t *mcfg);

esp_err_t iotmer_wifi_connect(void);

esp_err_t iotmer_nvs_load_creds(iotmer_creds_t *out);
esp_err_t iotmer_nvs_save_creds(const iotmer_creds_t *creds);

/**
 * @param https_performed  If non-NULL, set to true only when this call completed an HTTPS
 *                         provision (fresh JSON). False when auth code empty and session skipped.
 */
esp_err_t iotmer_provision(iotmer_creds_t *out, bool *https_performed);

/**
 * @param after_https_provision  If true, OTA runs even when firmware SHA matches NVS applied SHA
 *                                (same binary re-download after a successful HTTPS provision).
 */
esp_err_t iotmer_ota_apply_if_needed(iotmer_creds_t *creds, bool after_https_provision);

esp_err_t iotmer_topics_build_publish(char *out, size_t out_len,
                                      const char *workspace_slug,
                                      const char *device_key,
                                      const char *suffix);
esp_err_t iotmer_topics_subscribe_cmd(char *out, size_t out_len,
                                      const char *workspace_slug,
                                      const char *device_key);
esp_err_t iotmer_topics_subscribe_config(char *out, size_t out_len,
                                         const char *workspace_slug,
                                         const char *device_key);
bool iotmer_topics_match_cmd(const char *topic, const char *workspace_slug,
                             const char *device_key);
bool iotmer_topics_match_config(const char *topic, const char *workspace_slug,
                                const char *device_key);

/** Standard MQTT topic-filter match supporting `+` (single level) and `#` (multi level). */
bool iotmer_topic_filter_matches(const char *filter, const char *topic);

/**
 * Enqueue an MQTT publish for delivery by the mqtt client task.
 * Safe from any task, including the MQTT event handler.
 */
esp_err_t iotmer_mqtt_publish(iotmer_client_t *client,
                              const char *topic,
                              const char *payload,
                              int qos,
                              int retain);

/**
 * Build the JSON presence payload: {"status":"<status-lowercased>","ts":<unix s|0>}.
 * ts is 0 when the device clock has not been synced (pre-SNTP).
 */
esp_err_t iotmer_presence_build_payload(char *out, size_t out_len, const char *status);

/** Short creds critical section. Lock order: creds, then the failover lock. */
void iotmer_creds_lock(void);
void iotmer_creds_unlock(void);

/** true when the STA has an IP. Wi-Fi down must not advance the MQTT broker index. */
bool iotmer_wifi_sta_has_ip(void);

/**
 * TLS coexistence hooks. Nested acquire/release (MQTT session + primary probe)
 * suspends once and resumes when the last handshake finishes. A release with
 * nothing held is a no-op, so the existing double-release on teardown stays safe.
 */
void iotmer_tls_hook_acquire(iotmer_client_t *client, bool tls);
void iotmer_tls_hook_release(iotmer_client_t *client);

/**
 * Parse `mqtt_endpoints` / `policy` from a provision or mqtt-endpoints JSON object
 * (root or nested `data`). No array: keep a stored server list, or synthesize one
 * endpoint from `creds->mqtt_host`.
 */
void iotmer_mqtt_directory_ingest(struct cJSON *root, iotmer_creds_t *creds);

/** Load the endpoint blob from an open NVS handle. Missing blob synthesizes legacy. */
void iotmer_mqtt_directory_nvs_read(nvs_handle_t handle, iotmer_creds_t *creds);

/** Write the endpoint blob on an open handle. Does not commit. */
esp_err_t iotmer_mqtt_directory_nvs_write(nvs_handle_t handle);

/**
 * Bind failover to @p client and reset the per-session counters.
 * Does not clear the stored endpoint list, ETag, or last_good host.
 */
void iotmer_mqtt_failover_prepare(iotmer_client_t *client);

/**
 * Start the failover worker (boot GET, probes, daily refresh).
 * The task stays parked after disconnect so it is not created twice.
 */
esp_err_t iotmer_mqtt_failover_start(iotmer_client_t *client);

/** Idle the worker. Does not drop the stored endpoint list. */
void iotmer_mqtt_failover_stop(void);

/**
 * Copy the current endpoint. Caller holds the creds lock.
 * Hostnames only; a resolved address is never stored on the client.
 */
void iotmer_mqtt_failover_select_unlocked(const iotmer_client_t *client,
                                          char *host, size_t host_len,
                                          int *port, bool *tls,
                                          uint8_t *index, const char **role);

bool iotmer_mqtt_failover_uses_tls(const iotmer_client_t *client);

void iotmer_mqtt_failover_on_connack(iotmer_client_t *client);
void iotmer_mqtt_failover_on_error(const esp_mqtt_error_codes_t *err);

typedef struct {
    uint32_t wait_ms;
    bool arm_timer;
} iotmer_mqtt_fo_step_t;

void iotmer_mqtt_failover_on_disconnect(iotmer_client_t *client, iotmer_mqtt_fo_step_t *step);

void iotmer_mqtt_failover_suppress_disconnect(void);
bool iotmer_mqtt_failover_consume_suppress(void);
bool iotmer_mqtt_failover_consume_need_hard(void);
void iotmer_mqtt_failover_clear_need_hard(void);
void iotmer_mqtt_failover_drop_dns_cache(void);

#ifdef __cplusplus
}
#endif

