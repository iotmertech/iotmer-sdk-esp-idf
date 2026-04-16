#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "iotmer_client.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

