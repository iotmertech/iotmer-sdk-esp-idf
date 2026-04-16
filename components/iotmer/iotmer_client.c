/*
 * iotmer_client.c — init, connect, disconnect, pub/sub.
 *
 * Design rules enforced here:
 *  - No ESP_ERROR_CHECK; propagate errors via return codes.
 *  - No heap allocations; all state lives in the caller-provided iotmer_client_t.
 *  - Reconnect is manual (disable_auto_reconnect = true) via a one-shot esp_timer
 *    to avoid fighting with the MQTT stack's internal state machine.
 *  - Subscriptions are re-issued on every MQTT_EVENT_CONNECTED so they survive
 *    broker-side session resets (clean-session semantics).
 */

#include <errno.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

/*
 * Kconfig new option: stale build trees may not have regenerated sdkconfig.h yet.
 * Kconfig default is 8000 ms; keep the same fallback here.
 */
#ifndef CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS
#define CONFIG_IOTMER_FIRMWARE_POLL_AGGRESSIVE_MS 8000
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

static esp_err_t subscribe_commands_filter(iotmer_client_t *client);
static esp_err_t subscribe_config_filter(iotmer_client_t *client);

/* ------------------------------------------------------------------ */
/* MQTT event handler                                                   */
/* ------------------------------------------------------------------ */

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
         * Set the flag before subscribing so subscribe_suffix's
         * connected-guard passes.
         */
        client->connected = true;
        client->mqtt_auth_backoff_ms = 0;
        if (client->reconnect_timer) {
            (void)esp_timer_stop((esp_timer_handle_t)client->reconnect_timer);
        }
        ESP_LOGI(TAG, "MQTT connected");

        /*
         * Resubscribe after every (re)connect.
         * MQTT brokers with clean-session semantics drop all subscriptions
         * on disconnect, so we must reissue them here.
         */
        if (client->commands_cb) {
            esp_err_t e = subscribe_commands_filter(client);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "cmd/# resubscribe failed: %s", esp_err_to_name(e));
            }
        }
        if (client->config_cb) {
            esp_err_t e = subscribe_config_filter(client);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "config/# resubscribe failed: %s", esp_err_to_name(e));
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED: {
        client->connected = false;
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
        break;
    }

    case MQTT_EVENT_DATA: {
        if (event->topic_len <= 0) break;

        /* Copy topic into a null-terminated local buffer. */
        char topic[256];
        int tlen = (event->topic_len < (int)sizeof(topic) - 1)
                       ? event->topic_len
                       : (int)sizeof(topic) - 1;
        memcpy(topic, event->topic, (size_t)tlen);
        topic[tlen] = '\0';

        /* Console ACL: {workspace_slug}/{device_key}/cmd/... and .../config/... */
        if (client->commands_cb &&
            iotmer_topics_match_cmd(topic, client->creds.workspace_slug,
                                    client->creds.device_key)) {
            client->commands_cb(topic, event->data, event->data_len,
                                client->commands_ctx);
        } else if (client->config_cb &&
                   iotmer_topics_match_config(topic, client->creds.workspace_slug,
                                              client->creds.device_key)) {
            client->config_cb(topic, event->data, event->data_len,
                              client->config_ctx);
        }
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
    (void)esp_mqtt_client_reconnect((esp_mqtt_client_handle_t)client->mqtt);
}

/* ------------------------------------------------------------------ */
/* Publish / subscribe helpers                                          */
/* ------------------------------------------------------------------ */

static esp_err_t subscribe_commands_filter(iotmer_client_t *client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (!client->mqtt || !client->connected) return ESP_ERR_INVALID_STATE;

    char topic[256];
    esp_err_t err = iotmer_topics_subscribe_cmd(topic, sizeof(topic),
                                                client->creds.workspace_slug,
                                                client->creds.device_key);
    if (err != ESP_OK) return err;

    int msg_id = esp_mqtt_client_subscribe(
        (esp_mqtt_client_handle_t)client->mqtt, topic, 1 /* QoS 1 */);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "subscribe(%s) failed", topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "subscribed: %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
}

static esp_err_t subscribe_config_filter(iotmer_client_t *client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (!client->mqtt || !client->connected) return ESP_ERR_INVALID_STATE;

    char topic[256];
    esp_err_t err = iotmer_topics_subscribe_config(topic, sizeof(topic),
                                                   client->creds.workspace_slug,
                                                   client->creds.device_key);
    if (err != ESP_OK) return err;

    int msg_id = esp_mqtt_client_subscribe(
        (esp_mqtt_client_handle_t)client->mqtt, topic, 1 /* QoS 1 */);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "subscribe(%s) failed", topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "subscribed: %s (msg_id=%d)", topic, msg_id);
    return ESP_OK;
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
     * Create the reconnect timer before starting the MQTT client.
     * On any failure below we jump to fail_timer to clean it up, so
     * there is no path where a created timer leaks.
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
    };

    if (client->cfg.tls) {
        /* Use the bundled Mozilla CA root store; no custom cert needed. */
        mcfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_mqtt_client_handle_t h = esp_mqtt_client_init(&mcfg);
    if (!h) {
        err = ESP_ERR_NO_MEM;
        goto fail_timer;
    }

    err = esp_mqtt_client_register_event(h, ESP_EVENT_ANY_ID,
                                         mqtt_event_handler, client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event register failed: %s", esp_err_to_name(err));
        goto fail_client;
    }

    err = esp_mqtt_client_start(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed: %s", esp_err_to_name(err));
        goto fail_client;
    }

    client->mqtt = h;
    ESP_LOGI(TAG, "MQTT client started (host=%s port=%d tls=%s client_id=%s)",
             client->creds.mqtt_host, resolved_port,
             client->cfg.tls ? "yes" : "no", mqtt_client_id);
    return ESP_OK;

fail_client:
    esp_mqtt_client_destroy(h);
fail_timer:
    esp_timer_delete((esp_timer_handle_t)client->reconnect_timer);
    client->reconnect_timer = NULL;
    return err;
}

void iotmer_disconnect(iotmer_client_t *client)
{
    if (!client) return;

    /*
     * Stop the reconnect timer *before* marking the client disconnected
     * to avoid a race where MQTT_EVENT_DISCONNECTED fires during teardown
     * and re-arms the timer.
     */
    if (client->reconnect_timer) {
        (void)esp_timer_stop((esp_timer_handle_t)client->reconnect_timer);
    }

    client->connected = false;

    if (client->mqtt) {
        (void)esp_mqtt_client_stop((esp_mqtt_client_handle_t)client->mqtt);
        esp_mqtt_client_destroy((esp_mqtt_client_handle_t)client->mqtt);
        client->mqtt = NULL;
    }

    if (client->reconnect_timer) {
        esp_timer_delete((esp_timer_handle_t)client->reconnect_timer);
        client->reconnect_timer = NULL;
    }
}

esp_err_t iotmer_subscribe_commands(iotmer_client_t *client,
                                    iotmer_message_cb_t cb, void *ctx)
{
    if (!client || !cb) return ESP_ERR_INVALID_ARG;
    client->commands_cb  = cb;
    client->commands_ctx = ctx;
    return subscribe_commands_filter(client);
}

esp_err_t iotmer_subscribe_config(iotmer_client_t *client,
                                  iotmer_message_cb_t cb, void *ctx)
{
    if (!client || !cb) return ESP_ERR_INVALID_ARG;
    client->config_cb  = cb;
    client->config_ctx = ctx;
    return subscribe_config_filter(client);
}
