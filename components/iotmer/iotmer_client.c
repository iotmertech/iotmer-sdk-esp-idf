/*
 * iotmer_client.c — init, connect, disconnect, pub/sub.
 *
 * Design rules enforced here:
 *  - No ESP_ERROR_CHECK; propagate errors via return codes.
 *  - No steady-state heap allocations; all state lives in the caller-provided
 *    iotmer_client_t. (Exception: a bounded, on-demand reassembly buffer for
 *    MQTT messages larger than the esp-mqtt RX buffer — freed after dispatch.)
 *  - Reconnect is manual (disable_auto_reconnect = true) via a one-shot esp_timer
 *    to avoid fighting with the MQTT stack's internal state machine.
 *  - Subscriptions are re-issued on every MQTT_EVENT_CONNECTED so they survive
 *    broker-side session resets (clean-session semantics).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

/* TLS heap guard threshold (largest contiguous internal block). 0 disables. */
#ifndef CONFIG_IOTMER_TLS_MIN_HEAP_GUARD
#define CONFIG_IOTMER_TLS_MIN_HEAP_GUARD 0
#endif

/*
 * Max bytes for reassembling MQTT messages that esp-mqtt delivers in multiple
 * MQTT_EVENT_DATA fragments (message larger than CONFIG_MQTT_BUFFER_SIZE).
 * 0 disables reassembly: oversized messages are dropped with a warning.
 * Fallback for stale build trees that haven't regenerated sdkconfig.h.
 */
#ifndef CONFIG_IOTMER_MQTT_RX_ASSEMBLY_MAX
#define CONFIG_IOTMER_MQTT_RX_ASSEMBLY_MAX 8192
#endif

/*
 * Kconfig new option: stale build trees may not have regenerated sdkconfig.h yet.
 * Kconfig default is 8000 ms; keep the same fallback here.
 */
#ifndef CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS
#define CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS 8000
#endif

/*
 * MQTT outbox cap (bytes). QoS1/2 publishes queue in the outbox while the
 * connection is down; without a limit they can exhaust the heap on low-RAM
 * devices. 0 = esp-mqtt default (unbounded).
 */
#ifndef CONFIG_IOTMER_MQTT_OUTBOX_LIMIT
#define CONFIG_IOTMER_MQTT_OUTBOX_LIMIT 16384
#endif

#if CONFIG_IOTMER_FIRMWARE_POLL
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#endif

#include "iotmer_internal.h"

#define TAG "iotmer_client"

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Apply optional CONFIG_IOTMER_WORKSPACE_SLUG when slug is still empty after provision/NVS.
 * Normal path: provision JSON supplies workspace_slug; MQTT ACL topics use
 * {workspace_slug}/{device_key}/...
 */
static void ensure_workspace_slug(iotmer_creds_t *creds)
{
    if (!creds) return;
    if (creds->workspace_slug[0] != '\0') return; /* from API JSON or NVS */

    const char *slug = CONFIG_IOTMER_WORKSPACE_SLUG;
    if (slug[0] != '\0') {
        strncpy(creds->workspace_slug, slug, sizeof(creds->workspace_slug));
        creds->workspace_slug[sizeof(creds->workspace_slug) - 1] = '\0';
    } else {
        ESP_LOGD(TAG, "workspace_slug empty (expect provision API JSON; optional Kconfig override)");
    }
}

#if CONFIG_IOTMER_FIRMWARE_POLL

static TaskHandle_t s_fw_poll_task;
/* OTA rejected the downloaded image (wrong chip / corrupt header, etc.) — keep polling. */
static bool s_fw_poll_mismatch_retry;

static void fw_poll_note_ota_result(esp_err_t ota_err)
{
    if (ota_err == ESP_OK) {
        s_fw_poll_mismatch_retry = false;
        return;
    }
    s_fw_poll_mismatch_retry = true;
    if (ota_err == ESP_ERR_INVALID_VERSION || ota_err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGW(TAG, "OTA image rejected (%s) — firmware poll fast retry every %d ms",
                 esp_err_to_name(ota_err), CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS);
    } else {
        ESP_LOGW(TAG, "OTA failed (%s) — firmware poll will retry every %d ms",
                 esp_err_to_name(ota_err), CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS);
    }
}

static bool creds_need_firmware_poll(const iotmer_creds_t *c)
{
    if (!c || c->device_id[0] == '\0') {
        return false;
    }
    if (s_fw_poll_mismatch_retry) {
        return true;
    }
    if (c->firmware_url[0] != '\0' && c->firmware_checksum_sha256[0] != '\0') {
        return false;
    }
    /*
     * Missing firmware metadata can only be refreshed via HTTPS provision.
     * Without an auth code that path is skipped — avoid an infinite poll loop.
     */
    if (strlen(CONFIG_IOTMER_PROVISION_AUTH_CODE) == 0) {
        return false;
    }
    return true;
}

static void firmware_poll_stop(void)
{
    if (s_fw_poll_task) {
        TaskHandle_t h = s_fw_poll_task;
        s_fw_poll_task = NULL;
        vTaskDelete(h);
    }
}

static int firmware_poll_sleep_ms(const iotmer_client_t *cl)
{
    if (!cl) {
        return CONFIG_IOTMER_FIRMWARE_POLL_INTERVAL_MS;
    }
    if (s_fw_poll_mismatch_retry) {
        return CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS;
    }
    if (cl->creds.firmware_url[0] == '\0' || cl->creds.firmware_checksum_sha256[0] == '\0') {
        return CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS;
    }
    return CONFIG_IOTMER_FIRMWARE_POLL_INTERVAL_MS;
}

static void firmware_poll_task(void *arg)
{
    iotmer_client_t *client = (iotmer_client_t *)arg;
    /*
     * When OTA rejects the image but URL+SHA are already known, re-calling provision
     * every interval hammers HTTPS + WiFi on single-core targets and can trip the
     * idle task watchdog. Re-pull API metadata only every N rounds; other rounds OTA-only.
     */
    unsigned mismatch_round = 0;
    bool first_loop = true;

    for (;;) {
        if (first_loop) {
            first_loop = false;
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(firmware_poll_sleep_ms(client)));
        }

        if (!client || !creds_need_firmware_poll(&client->creds)) {
            ESP_LOGI(TAG, "firmware poll: done (metadata OK and no OTA mismatch retry)");
            break;
        }

        /* Let IDLE / WiFi / lwIP run before a long TLS session. */
        vTaskDelay(pdMS_TO_TICKS(200));

        bool run_provision = true;
        if (s_fw_poll_mismatch_retry &&
            client->creds.firmware_url[0] != '\0' &&
            client->creds.firmware_checksum_sha256[0] != '\0') {
            mismatch_round++;
            run_provision = (mismatch_round % 5u == 1u);
            if (!run_provision) {
                ESP_LOGI(TAG, "firmware poll: OTA-only retry (mismatch round %u)", mismatch_round);
            }
        } else {
            mismatch_round = 0;
        }

        esp_err_t e = ESP_OK;
        bool provision_https = false;
        if (run_provision) {
            ESP_LOGI(TAG, "firmware poll: re-provisioning (next sleep=%d ms)",
                     firmware_poll_sleep_ms(client));
            e = iotmer_provision(&client->creds, &provision_https);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "firmware poll provision failed: %s", esp_err_to_name(e));
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ensure_workspace_slug(&client->creds);
            e = iotmer_nvs_save_creds(&client->creds);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "firmware poll NVS save: %s", esp_err_to_name(e));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));

        e = iotmer_ota_apply_if_needed(&client->creds, provision_https);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "firmware poll OTA: %s", esp_err_to_name(e));
        }
        fw_poll_note_ota_result(e);

        vTaskDelay(pdMS_TO_TICKS(50));

        if (!creds_need_firmware_poll(&client->creds)) {
            ESP_LOGI(TAG, "firmware poll: stopping poll task");
            break;
        }
    }

    s_fw_poll_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t firmware_poll_start(iotmer_client_t *client)
{
    if (!client || !creds_need_firmware_poll(&client->creds)) {
        return ESP_OK;
    }

    firmware_poll_stop();

    BaseType_t ok = xTaskCreate(
        firmware_poll_task,
        "iotmer_fwpoll",
        CONFIG_IOTMER_FIRMWARE_POLL_TASK_STACK,
        client,
        CONFIG_IOTMER_FIRMWARE_POLL_TASK_PRIORITY,
        &s_fw_poll_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "firmware poll task create failed");
        s_fw_poll_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "firmware poll task started (interval=%d ms)",
             CONFIG_IOTMER_FIRMWARE_POLL_INTERVAL_MS);
    return ESP_OK;
}

#endif /* CONFIG_IOTMER_FIRMWARE_POLL */

static esp_err_t mqtt_client_start(iotmer_client_t *client);
static esp_err_t resubscribe_all(iotmer_client_t *client);

/* ------------------------------------------------------------------ */
/* Broker activity tracking (phantom detection)                         */
/* ------------------------------------------------------------------ */

static void mark_broker_activity(iotmer_client_t *client)
{
    if (client) {
        client->last_broker_activity_us = esp_timer_get_time();
    }
}

/* ------------------------------------------------------------------ */
/* MQTT event handler                                                   */
/* ------------------------------------------------------------------ */

static void dispatch_data(iotmer_client_t *client, const char *topic,
                          const char *data, int data_len)
{
    for (int i = 0; i < IOTMER_MAX_SUBSCRIPTIONS; ++i) {
        iotmer_subscription_t *s = &client->subs[i];
        if (s->in_use && s->cb && iotmer_topic_filter_matches(s->filter, topic)) {
            s->cb(topic, data, data_len, s->ctx);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Fragmented (oversized) MQTT message reassembly                       */
/* ------------------------------------------------------------------ */

static void rx_assembly_reset(iotmer_client_t *client)
{
    if (client->rx_assembly) {
        free(client->rx_assembly);
        client->rx_assembly = NULL;
    }
    client->rx_assembly_total = 0;
    client->rx_assembly_received = 0;
    client->rx_assembly_dropping = false;
    client->rx_assembly_topic[0] = '\0';
}

/*
 * esp-mqtt delivers messages larger than its RX buffer as multiple
 * MQTT_EVENT_DATA events: the first carries the topic and offset 0; follow-ups
 * carry topic_len == 0 and an advancing current_data_offset. Reassemble them
 * into one buffer (bounded by CONFIG_IOTMER_MQTT_RX_ASSEMBLY_MAX) so callbacks
 * always receive complete payloads — never a broken JSON fragment.
 */
static void handle_fragmented_data(iotmer_client_t *client, esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        /* First fragment: (re)start assembly. Any previous half message is stale. */
        rx_assembly_reset(client);

        if (event->topic_len <= 0) {
            client->rx_assembly_dropping = true;
            return;
        }
        if (CONFIG_IOTMER_MQTT_RX_ASSEMBLY_MAX <= 0 ||
            event->total_data_len > CONFIG_IOTMER_MQTT_RX_ASSEMBLY_MAX) {
            ESP_LOGW(TAG,
                     "oversized MQTT message dropped (total=%d > limit=%d) — increase "
                     "CONFIG_MQTT_BUFFER_SIZE or IOTMER_MQTT_RX_ASSEMBLY_MAX",
                     event->total_data_len, CONFIG_IOTMER_MQTT_RX_ASSEMBLY_MAX);
            client->rx_assembly_dropping = true;
            return;
        }

        client->rx_assembly = (char *)malloc((size_t)event->total_data_len);
        if (!client->rx_assembly) {
            ESP_LOGW(TAG, "oversized MQTT message dropped (total=%d) — no heap for reassembly",
                     event->total_data_len);
            client->rx_assembly_dropping = true;
            return;
        }

        int tlen = (event->topic_len < (int)sizeof(client->rx_assembly_topic) - 1)
                       ? event->topic_len
                       : (int)sizeof(client->rx_assembly_topic) - 1;
        memcpy(client->rx_assembly_topic, event->topic, (size_t)tlen);
        client->rx_assembly_topic[tlen] = '\0';
        client->rx_assembly_total = event->total_data_len;
    } else if (client->rx_assembly_dropping) {
        /* Silently consume the remaining fragments of a dropped message. */
        if (event->current_data_offset + event->data_len >= event->total_data_len) {
            rx_assembly_reset(client);
        }
        return;
    } else if (!client->rx_assembly ||
               event->total_data_len != client->rx_assembly_total ||
               event->current_data_offset != client->rx_assembly_received) {
        ESP_LOGW(TAG, "MQTT fragment sequence mismatch (offset=%d expected=%d) — message dropped",
                 event->current_data_offset, client->rx_assembly_received);
        rx_assembly_reset(client);
        client->rx_assembly_dropping =
            (event->current_data_offset + event->data_len < event->total_data_len);
        return;
    }

    if (event->data_len > 0) {
        memcpy(client->rx_assembly + client->rx_assembly_received,
               event->data, (size_t)event->data_len);
        client->rx_assembly_received += event->data_len;
    }

    if (client->rx_assembly_received >= client->rx_assembly_total) {
        dispatch_data(client, client->rx_assembly_topic,
                      client->rx_assembly, client->rx_assembly_total);
        rx_assembly_reset(client);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)base;

    iotmer_client_t *client = (iotmer_client_t *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (!client || !event) return;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        /*
         * Stop any pending reconnect timer; we are now connected.
         * Set the flag before subscribing so the connected-guard passes.
         */
        client->connected = true;
        client->mqtt_auth_backoff_ms = 0;
        mark_broker_activity(client);
        if (client->reconnect_timer) {
            (void)esp_timer_stop((esp_timer_handle_t)client->reconnect_timer);
        }
        ESP_LOGI(TAG, "MQTT connected");

        /* Handshake done: let the app restore any resources freed for TLS. */
        if (client->cfg.on_tls_release) {
            client->cfg.on_tls_release(client->cfg.user_ctx);
        }

        /* Presence/LWT: on every connect, publish retained online JSON. */
        if (client->cfg.presence_lwt_enable) {
            esp_err_t e = iotmer_presence_publish(client, "online");
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "presence online publish failed: %s", esp_err_to_name(e));
            }
        }

        /*
         * Re-issue all registered subscriptions after every (re)connect.
         * MQTT brokers with clean-session semantics drop subscriptions on
         * disconnect, so we must reissue them here.
         */
        (void)resubscribe_all(client);

        if (client->cfg.on_connected) {
            client->cfg.on_connected(client, client->cfg.user_ctx);
        }
        break;

    case MQTT_EVENT_DISCONNECTED: {
        client->connected = false;
        /* Any half-reassembled message from this session is now stale. */
        rx_assembly_reset(client);
        uint32_t wait_ms = client->mqtt_auth_backoff_ms != 0U
                               ? client->mqtt_auth_backoff_ms
                               : (uint32_t)client->cfg.reconnect_delay_ms;
        if (client->mqtt_auth_backoff_ms != 0U) {
            ESP_LOGW(TAG, "MQTT disconnected — reconnecting in %u ms (auth-failure backoff)",
                     wait_ms);
        } else {
            ESP_LOGW(TAG, "MQTT disconnected — reconnecting in %u ms", wait_ms);
        }
        if (client->reconnect_timer) {
            (void)esp_timer_stop((esp_timer_handle_t)client->reconnect_timer);
            (void)esp_timer_start_once((esp_timer_handle_t)client->reconnect_timer,
                                        (uint64_t)wait_ms * 1000ULL);
        }
        if (client->cfg.on_disconnected) {
            client->cfg.on_disconnected(client, client->cfg.user_ctx);
        }
        break;
    }

    case MQTT_EVENT_SUBSCRIBED: {
        mark_broker_activity(client);
        /* For SUBACK, esp-mqtt exposes the granted QoS byte(s) in event->data. */
        int rc = (event->data && event->data_len > 0) ? (int)(uint8_t)event->data[0] : 0;
        if (client->cfg.on_subscribed) {
            client->cfg.on_subscribed(client, event->msg_id, rc, client->cfg.user_ctx);
        }
        break;
    }

    case MQTT_EVENT_PUBLISHED:
        mark_broker_activity(client);
        if (client->cfg.on_published) {
            client->cfg.on_published(client, event->msg_id, client->cfg.user_ctx);
        }
        break;

    case MQTT_EVENT_DATA: {
        mark_broker_activity(client);

        /*
         * Message larger than the esp-mqtt RX buffer: delivered as multiple
         * fragments (2nd+ fragment has topic_len == 0). Reassemble before
         * dispatching so callbacks never see partial payloads.
         */
        if (event->current_data_offset != 0 ||
            event->data_len != event->total_data_len) {
            handle_fragmented_data(client, event);
            break;
        }

        if (event->topic_len <= 0) break;

        /* Copy topic into a null-terminated local buffer. */
        char topic[256];
        int tlen = (event->topic_len < (int)sizeof(topic) - 1)
                       ? event->topic_len
                       : (int)sizeof(topic) - 1;
        memcpy(topic, event->topic, (size_t)tlen);
        topic[tlen] = '\0';

        dispatch_data(client, topic, event->data, event->data_len);
        break;
    }

    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            const esp_mqtt_error_codes_t *eh = event->error_handle;
            const char *etype = "other";
            if (eh->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                etype = "TCP/TLS transport";
            } else if (eh->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                etype = "CONNACK refused";
            } else if (eh->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
                etype = "SUBSCRIBE failed";
            }
            if (eh->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                int rc = (int)eh->connect_return_code;
                /*
                 * MQTT 3.1.1: 4 = bad username/password, 5 = not authorized.
                 * Hammering reconnect every few seconds triggers EMQX flapping / bans.
                 */
                if (rc == 4 || rc == 5) {
                    uint32_t b = client->mqtt_auth_backoff_ms;
                    if (b == 0U) {
                        b = (uint32_t)client->cfg.reconnect_delay_ms * 6U;
                        if (b < 30000U) {
                            b = 30000U;
                        }
                    } else if (b < 300000U) {
                        b *= 2U;
                        if (b > 300000U) {
                            b = 300000U;
                        }
                    }
                    client->mqtt_auth_backoff_ms = b;
                    ESP_LOGW(TAG,
                             "MQTT CONNACK auth failure (rc=%d) — next reconnect uses %u ms backoff "
                             "(refresh NVS credentials via HTTPS provision if this persists)",
                             rc, b);
                    if (client->cfg.on_auth_rejected) {
                        client->cfg.on_auth_rejected(client, rc, client->cfg.user_ctx);
                    }
                }
                ESP_LOGE(TAG,
                         "MQTT error: %s (type=%d) CONNACK_rc=%d "
                         "(MQTT3: 3=server unavailable, 4=bad user/pass, 5=not authorized) "
                         "sock_errno=%d (%s) tls_esp_err=%s stack_err=%d",
                         etype, (int)eh->error_type, rc,
                         eh->esp_transport_sock_errno,
                         eh->esp_transport_sock_errno != 0
                             ? strerror(eh->esp_transport_sock_errno)
                             : "n/a",
                         esp_err_to_name(eh->esp_tls_last_esp_err), eh->esp_tls_stack_err);
            } else {
                ESP_LOGE(TAG,
                         "MQTT error: %s (type=%d) sock_errno=%d (%s) tls_esp_err=%s stack_err=%d",
                         etype, (int)eh->error_type, eh->esp_transport_sock_errno,
                         eh->esp_transport_sock_errno != 0
                             ? strerror(eh->esp_transport_sock_errno)
                             : "n/a",
                         esp_err_to_name(eh->esp_tls_last_esp_err), eh->esp_tls_stack_err);
            }
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Reconnect timer callback                                             */
/* ------------------------------------------------------------------ */

static void reconnect_timer_cb(void *arg)
{
    iotmer_client_t *client = (iotmer_client_t *)arg;
    if (!client || !client->mqtt) return;
    ESP_LOGI(TAG, "Attempting MQTT reconnect...");
    esp_err_t e = esp_mqtt_client_reconnect((esp_mqtt_client_handle_t)client->mqtt);
    if (e != ESP_OK) {
        /*
         * esp_mqtt_client_reconnect() returns an error on a dead/half-open socket and
         * `connected` never clears — a soft reconnect cannot recover. Fall back to a full
         * teardown + fresh start.
         */
        ESP_LOGW(TAG, "soft reconnect failed (%s) — performing hard reconnect",
                 esp_err_to_name(e));
        (void)iotmer_reconnect_hard(client);
    }
}

static void phantom_timer_cb(void *arg)
{
    iotmer_client_t *client = (iotmer_client_t *)arg;
    if (!client || !client->connected) return;
    if (client->cfg.phantom_timeout_ms == 0U) return;

    int64_t idle_ms = iotmer_ms_since_broker_activity(client);
    if (idle_ms < 0) return;
    if ((uint32_t)idle_ms <= client->cfg.phantom_timeout_ms) return;

    ESP_LOGW(TAG, "PHANTOM connection: %lld ms with no broker activity — hard reconnect",
             (long long)idle_ms);
    if (client->cfg.on_phantom_detected) {
        client->cfg.on_phantom_detected(client, client->cfg.user_ctx);
    }
    (void)iotmer_reconnect_hard(client);
}

static uint32_t phantom_check_period_ms(uint32_t timeout_ms)
{
    uint32_t p = timeout_ms / 4U;
    if (p < 2000U) p = 2000U;
    if (p > 15000U) p = 15000U;
    return p;
}

/* ------------------------------------------------------------------ */
/* Publish / subscribe helpers                                          */
/* ------------------------------------------------------------------ */

/* Issue every registered subscription to the broker (used on each CONNECTED). */
static esp_err_t resubscribe_all(iotmer_client_t *client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (!client->mqtt || !client->connected) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < IOTMER_MAX_SUBSCRIPTIONS; ++i) {
        iotmer_subscription_t *s = &client->subs[i];
        if (!s->in_use) continue;

        int msg_id = esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)client->mqtt, s->filter, s->qos);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "resubscribe(%s) failed", s->filter);
            ret = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "subscribed: %s qos=%d (msg_id=%d)", s->filter, s->qos, msg_id);
        }
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t iotmer_init(iotmer_client_t *client, const iotmer_config_t *cfg)
{
    if (!client || !cfg) return ESP_ERR_INVALID_ARG;

#if CONFIG_IOTMER_FIRMWARE_POLL
    firmware_poll_stop();
    s_fw_poll_mismatch_retry = false;
#endif

    memset(client, 0, sizeof(*client));
    client->cfg = *cfg;

    /* Connect WiFi — blocks until IP acquired or timeout. */
    esp_err_t err = iotmer_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Load prior state from NVS (optional on first boot). */
    err = iotmer_nvs_load_creds(&client->creds);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS load failed: %s — starting with empty creds",
                     esp_err_to_name(err));
        }
        memset(&client->creds, 0, sizeof(client->creds));
    }

    /*
     * Provision: HTTPS registration when IOTMER_PROVISION_AUTH_CODE is set; otherwise
     * iotmer_provision() may skip the HTTP round-trip if NVS already holds a full session.
     */
    if (client->creds.device_id[0] != '\0') {
        ESP_LOGI(TAG, "provision with existing device_id=%s", client->creds.device_id);
    } else {
        ESP_LOGI(TAG, "provision (first registration, no device_id yet)");
    }

    bool provision_https = false;
    err = iotmer_provision(&client->creds, &provision_https);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning failed: %s", esp_err_to_name(err));
        return err;
    }

    ensure_workspace_slug(&client->creds);

    err = iotmer_nvs_save_creds(&client->creds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS save failed (%s) — credentials may be stale on next boot",
                 esp_err_to_name(err));
    }

    err = iotmer_ota_apply_if_needed(&client->creds, provision_https);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA step failed: %s — continuing without reboot",
                 esp_err_to_name(err));
    }

#if CONFIG_IOTMER_FIRMWARE_POLL
    fw_poll_note_ota_result(err);
    err = firmware_poll_start(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "firmware poll could not start: %s", esp_err_to_name(err));
    }
#endif

    ESP_LOGI(TAG, "init complete (device_id=%s, mqtt_host=%s)",
             client->creds.device_id, client->creds.mqtt_host);
    return ESP_OK;
}

/*
 * Build + init + start the esp-mqtt client (no timer management here so it can be
 * reused by both iotmer_connect() and iotmer_reconnect_hard()).
 */
static esp_err_t mqtt_client_start(iotmer_client_t *client)
{
    int resolved_port = client->creds.mqtt_port
                            ? client->creds.mqtt_port
                            : client->cfg.broker_port;

    /*
     * ESP-IDF defaults client_id to a MAC-derived value; many cloud brokers
     * authorize only when client_id matches the provisioned identity (often
     * the same string as mqtt_username).
     */
    const char *mqtt_client_id = (client->creds.mqtt_username[0] != '\0')
                                     ? client->creds.mqtt_username
                                     : client->creds.device_id;

    /*
     * TLS coexistence (A1): give the app a chance to free contiguous internal RAM
     * (e.g. suspend BLE) before the handshake. Optionally guard against an obviously
     * insufficient heap and fail early with a clear error instead of an opaque
     * mbedTLS -0x3000.
     */
    if (client->cfg.tls && client->cfg.on_tls_acquire) {
        client->cfg.on_tls_acquire(client->cfg.user_ctx);
    }
#if CONFIG_IOTMER_TLS_MIN_HEAP_GUARD > 0
    if (client->cfg.tls) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest < (size_t)CONFIG_IOTMER_TLS_MIN_HEAP_GUARD) {
            /* Give any just-triggered release (BLE deinit) a moment to settle, then re-check. */
            vTaskDelay(pdMS_TO_TICKS(150));
            largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        }
        if (largest < (size_t)CONFIG_IOTMER_TLS_MIN_HEAP_GUARD) {
            ESP_LOGE(TAG, "TLS heap guard: largest free internal block %u < %d — aborting connect",
                     (unsigned)largest, CONFIG_IOTMER_TLS_MIN_HEAP_GUARD);
            if (client->cfg.on_tls_release) {
                client->cfg.on_tls_release(client->cfg.user_ctx);
            }
            return ESP_ERR_NO_MEM;
        }
    }
#endif

    esp_mqtt_client_config_t mcfg = {
        .broker.address.hostname              = client->creds.mqtt_host,
        .broker.address.port                  = (uint32_t)resolved_port,
        .broker.address.transport             = client->cfg.tls
                                                    ? MQTT_TRANSPORT_OVER_SSL
                                                    : MQTT_TRANSPORT_OVER_TCP,
        .credentials.username                 = client->creds.mqtt_username,
        .credentials.authentication.password  = client->creds.mqtt_password,
        .credentials.client_id                = mqtt_client_id,
        .session.keepalive                    = client->cfg.keepalive_sec,
        .network.disable_auto_reconnect       = true, /* we handle reconnect manually */
#if CONFIG_IOTMER_MQTT_OUTBOX_LIMIT > 0
        .outbox.limit                         = CONFIG_IOTMER_MQTT_OUTBOX_LIMIT,
#endif
    };

    if (client->cfg.presence_lwt_enable) {
        /* MQTT LWT must reference memory that lives at least until client stop/destroy. */
        esp_err_t err = iotmer_topics_build_publish(client->presence_topic,
                                                    sizeof(client->presence_topic),
                                                    client->creds.workspace_slug,
                                                    client->creds.device_key,
                                                    "presence");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "presence topic build failed: %s", esp_err_to_name(err));
            return err;
        }

        /* Same JSON contract as iotmer_presence_publish(); ts=0 because the
         * broker delivers the will long after this config is built. */
        mcfg.session.last_will.topic  = client->presence_topic;
        mcfg.session.last_will.msg    = "{\"status\":\"offline\",\"ts\":0}";
        mcfg.session.last_will.qos    = 1;
        mcfg.session.last_will.retain = 1;
    }

    if (client->cfg.tls) {
        /* Use the bundled Mozilla CA root store; no custom cert needed. */
        mcfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_mqtt_client_handle_t h = esp_mqtt_client_init(&mcfg);
    if (!h) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_mqtt_client_register_event(h, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event register failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(h);
        return err;
    }

    err = esp_mqtt_client_start(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(h);
        return err;
    }

    client->mqtt = h;
    ESP_LOGI(TAG, "MQTT client started (host=%s port=%d tls=%s client_id=%s)",
             client->creds.mqtt_host, resolved_port,
             client->cfg.tls ? "yes" : "no", mqtt_client_id);
    return ESP_OK;
}

esp_err_t iotmer_connect(iotmer_client_t *client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (client->mqtt) return ESP_OK; /* already started */

    if (client->creds.mqtt_host[0] == '\0' ||
        client->creds.mqtt_username[0] == '\0' ||
        client->creds.device_id[0] == '\0' ||
        client->creds.workspace_slug[0] == '\0' ||
        client->creds.device_key[0] == '\0') {
        ESP_LOGE(TAG, "iotmer_connect called before successful iotmer_init (need MQTT creds + "
                      "workspace_slug + device_key for topics)");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Create the reconnect timer (and optional phantom watchdog) before starting the
     * MQTT client. On any failure below we clean them up, so no timer leaks.
     */
    esp_timer_create_args_t tcfg = {
        .callback              = &reconnect_timer_cb,
        .arg                   = client,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "iotmer_rc",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&tcfg,
        (esp_timer_handle_t *)&client->reconnect_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed: %s", esp_err_to_name(err));
        return err;
    }

    if (client->cfg.phantom_timeout_ms > 0U) {
        esp_timer_create_args_t pcfg = {
            .callback              = &phantom_timer_cb,
            .arg                   = client,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "iotmer_ph",
            .skip_unhandled_events = true,
        };
        err = esp_timer_create(&pcfg, (esp_timer_handle_t *)&client->phantom_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "phantom timer create failed: %s", esp_err_to_name(err));
            goto fail_timer;
        }
    }

    err = mqtt_client_start(client);
    if (err != ESP_OK) {
        goto fail_phantom;
    }

    if (client->phantom_timer) {
        uint32_t period = phantom_check_period_ms(client->cfg.phantom_timeout_ms);
        (void)esp_timer_start_periodic((esp_timer_handle_t)client->phantom_timer,
                                       (uint64_t)period * 1000ULL);
        ESP_LOGI(TAG, "phantom watchdog armed (timeout=%u ms, check=%u ms)",
                 client->cfg.phantom_timeout_ms, period);
    }
    return ESP_OK;

fail_phantom:
    if (client->phantom_timer) {
        esp_timer_delete((esp_timer_handle_t)client->phantom_timer);
        client->phantom_timer = NULL;
    }
fail_timer:
    esp_timer_delete((esp_timer_handle_t)client->reconnect_timer);
    client->reconnect_timer = NULL;
    return err;
}

esp_err_t iotmer_reconnect_hard(iotmer_client_t *client)
{
    if (!client) return ESP_ERR_INVALID_ARG;

    ESP_LOGW(TAG, "hard reconnect: tearing down MQTT client");

    /* Stop the one-shot reconnect timer (do NOT touch the phantom timer: this may be
     * called from its own callback context). */
    if (client->reconnect_timer) {
        (void)esp_timer_stop((esp_timer_handle_t)client->reconnect_timer);
    }

    client->connected = false;

    if (client->mqtt) {
        (void)esp_mqtt_client_stop((esp_mqtt_client_handle_t)client->mqtt);
        esp_mqtt_client_destroy((esp_mqtt_client_handle_t)client->mqtt);
        client->mqtt = NULL;
    }

    /* Brief settle so sockets / TLS buffers are fully released before re-init. */
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hard reconnect: restart failed (%s) — arming reconnect timer",
                 esp_err_to_name(err));
        if (client->reconnect_timer) {
            uint32_t wait_ms = client->cfg.reconnect_delay_ms > 0
                                   ? (uint32_t)client->cfg.reconnect_delay_ms
                                   : 5000U;
            (void)esp_timer_start_once((esp_timer_handle_t)client->reconnect_timer,
                                       (uint64_t)wait_ms * 1000ULL);
        }
    }
    return err;
}

int64_t iotmer_ms_since_broker_activity(const iotmer_client_t *client)
{
    if (!client || client->last_broker_activity_us == 0) {
        return -1;
    }
    int64_t now = esp_timer_get_time();
    return (now - client->last_broker_activity_us) / 1000;
}

esp_err_t iotmer_wifi_up(void)
{
    return iotmer_wifi_connect();
}

void iotmer_finalize_provisioning(iotmer_client_t *client, bool reboot, uint32_t reboot_delay_ms)
{
    if (client && client->mqtt) {
        iotmer_disconnect(client);
    }
    if (reboot) {
        ESP_LOGI(TAG, "finalize_provisioning: rebooting in %u ms", reboot_delay_ms);
        if (reboot_delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(reboot_delay_ms));
        }
        esp_restart();
    }
}

void iotmer_disconnect(iotmer_client_t *client)
{
    if (!client) return;

    /*
     * Stop the timers *before* marking the client disconnected to avoid a race where
     * MQTT_EVENT_DISCONNECTED fires during teardown and re-arms the reconnect timer.
     */
    if (client->reconnect_timer) {
        (void)esp_timer_stop((esp_timer_handle_t)client->reconnect_timer);
    }
    if (client->phantom_timer) {
        (void)esp_timer_stop((esp_timer_handle_t)client->phantom_timer);
    }

    client->connected = false;

    if (client->mqtt) {
        (void)esp_mqtt_client_stop((esp_mqtt_client_handle_t)client->mqtt);
        esp_mqtt_client_destroy((esp_mqtt_client_handle_t)client->mqtt);
        client->mqtt = NULL;
    }

    rx_assembly_reset(client);

    if (client->reconnect_timer) {
        esp_timer_delete((esp_timer_handle_t)client->reconnect_timer);
        client->reconnect_timer = NULL;
    }
    if (client->phantom_timer) {
        esp_timer_delete((esp_timer_handle_t)client->phantom_timer);
        client->phantom_timer = NULL;
    }
}

esp_err_t iotmer_subscribe(iotmer_client_t *client, const char *topic_filter, int qos,
                           iotmer_message_cb_t cb, void *ctx)
{
    if (!client || !topic_filter || topic_filter[0] == '\0' || !cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(topic_filter) >= IOTMER_TOPIC_FILTER_MAX) {
        ESP_LOGE(TAG, "subscribe: filter too long (%s)", topic_filter);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Update in place if the same filter is already registered, else take a free slot. */
    iotmer_subscription_t *slot = NULL;
    for (int i = 0; i < IOTMER_MAX_SUBSCRIPTIONS; ++i) {
        if (client->subs[i].in_use && strcmp(client->subs[i].filter, topic_filter) == 0) {
            slot = &client->subs[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < IOTMER_MAX_SUBSCRIPTIONS; ++i) {
            if (!client->subs[i].in_use) {
                slot = &client->subs[i];
                break;
            }
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "subscribe: registry full (max %d)", IOTMER_MAX_SUBSCRIPTIONS);
        return ESP_ERR_NO_MEM;
    }

    strncpy(slot->filter, topic_filter, sizeof(slot->filter) - 1);
    slot->filter[sizeof(slot->filter) - 1] = '\0';
    slot->qos = qos;
    slot->cb = cb;
    slot->ctx = ctx;
    slot->in_use = true;

    if (client->mqtt && client->connected) {
        int msg_id = esp_mqtt_client_subscribe(
            (esp_mqtt_client_handle_t)client->mqtt, slot->filter, slot->qos);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "subscribe(%s) failed", slot->filter);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "subscribed: %s qos=%d (msg_id=%d)", slot->filter, slot->qos, msg_id);
    } else {
        ESP_LOGI(TAG, "subscribe queued (not connected): %s", slot->filter);
    }
    return ESP_OK;
}

esp_err_t iotmer_unsubscribe(iotmer_client_t *client, const char *topic_filter)
{
    if (!client || !topic_filter) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < IOTMER_MAX_SUBSCRIPTIONS; ++i) {
        iotmer_subscription_t *s = &client->subs[i];
        if (s->in_use && strcmp(s->filter, topic_filter) == 0) {
            if (client->mqtt && client->connected) {
                (void)esp_mqtt_client_unsubscribe(
                    (esp_mqtt_client_handle_t)client->mqtt, s->filter);
            }
            memset(s, 0, sizeof(*s));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t iotmer_subscribe_commands(iotmer_client_t *client,
                                    iotmer_message_cb_t cb, void *ctx)
{
    if (!client || !cb) return ESP_ERR_INVALID_ARG;

    char topic[IOTMER_TOPIC_FILTER_MAX];
    esp_err_t err = iotmer_topics_subscribe_cmd(topic, sizeof(topic),
                                                client->creds.workspace_slug,
                                                client->creds.device_key);
    if (err != ESP_OK) return err;
    return iotmer_subscribe(client, topic, 1 /* QoS 1 */, cb, ctx);
}

esp_err_t iotmer_subscribe_config(iotmer_client_t *client,
                                  iotmer_message_cb_t cb, void *ctx)
{
    if (!client || !cb) return ESP_ERR_INVALID_ARG;

    char topic[IOTMER_TOPIC_FILTER_MAX];
    esp_err_t err = iotmer_topics_subscribe_config(topic, sizeof(topic),
                                                   client->creds.workspace_slug,
                                                   client->creds.device_key);
    if (err != ESP_OK) return err;
    return iotmer_subscribe(client, topic, 1 /* QoS 1 */, cb, ctx);
}
