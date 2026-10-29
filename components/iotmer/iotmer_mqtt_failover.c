/*
 * iotmer_mqtt_failover.c — one MQTT session, ordered broker list.
 *
 * mqtt_host / mqtt_port / mqtt_tls stay the first endpoint so older firmware
 * keeps working. A server list (mqtt_endpoints + policy) is stored beside
 * those fields in NVS key "mqtt_route".
 *
 * Leave a host only when Wi-Fi is up, the same host has failed at least
 * same_host_attempts times, and those failures span min_fail_window_sec.
 * Floors: window 300 s, failback health 300 s, 3 attempts, probe every
 * 15–60 s. DNS results are not kept across attempts.
 *
 * While the session is on a backup, probe the primary with a TLS handshake
 * (no MQTT CONNECT) and close that socket immediately. One failed probe
 * restarts the health window. A failed primary CONNECT returns to the same
 * backup.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "iotmer_internal.h"

#define TAG "iotmer_failover"

#define IOTMER_MQTT_ENDPOINT_MAX 4
#define IOTMER_MQTT_HOST_MAX 128
#define IOTMER_MQTT_ETAG_MAX 96
#define IOTMER_MQTT_DIR_VER 1
#define IOTMER_MQTT_NVS_KEY "mqtt_route"

#define FLOOR_FAIL_WINDOW_SEC 300u
#define FLOOR_FAILBACK_HEALTH_SEC 300u
#define FLOOR_SAME_HOST_ATTEMPTS 3u
#define PROBE_INTERVAL_MIN_SEC 15u
#define PROBE_INTERVAL_MAX_SEC 60u
#define DEFAULT_PROBE_INTERVAL_SEC 30u
#define DEFAULT_JITTER_SEC 60u
#define JITTER_CAP_SEC 86400u

#define PROBE_TIMEOUT_MS 4000
#define REFRESH_BODY_MAX 3072
#define DAY_US (24LL * 60LL * 60LL * 1000000LL)
#define HOUR_US (60LL * 60LL * 1000000LL)
#define TASK_STACK 10240
#define TASK_PRIO 3

#define ROLE_PRIMARY 1u
#define ROLE_BACKUP 2u

enum {
    LATCH_NONE = 0,
    LATCH_TRANSPORT,
    LATCH_ID_REJECTED,
    LATCH_AUTH,
    LATCH_PROTOCOL,
};

typedef struct __attribute__((packed)) {
    uint16_t port;
    uint8_t tls;
    uint8_t role;
    char host[IOTMER_MQTT_HOST_MAX];
} mqtt_ep_t;

typedef struct __attribute__((packed)) {
    uint8_t ver;
    uint8_t count;
    uint8_t from_server_list;
    uint8_t last_good_valid;
    uint16_t last_good_port;
    uint16_t reserved;
    char etag[IOTMER_MQTT_ETAG_MAX];
    char last_good_host[IOTMER_MQTT_HOST_MAX];
    uint32_t min_fail_window_sec;
    uint32_t same_host_attempts;
    uint32_t failback_healthy_sec;
    uint32_t probe_interval_sec;
    uint32_t failback_jitter_sec;
    mqtt_ep_t ep[IOTMER_MQTT_ENDPOINT_MAX];
} mqtt_route_blob_t;

_Static_assert(sizeof(IOTMER_MQTT_NVS_KEY) <= 16, "NVS key mqtt_route");
_Static_assert(sizeof(mqtt_route_blob_t) < 1900, "mqtt route blob");

enum {
    REFRESH_BOOT = 1,
    REFRESH_CYCLE,
    REFRESH_DAILY,
};

static SemaphoreHandle_t s_lock;
static mqtt_route_blob_t s_dir;
static iotmer_client_t *s_client;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_boot_sem;

static uint8_t s_index;
static uint32_t s_attempt_count;
static int64_t s_window_start_us;
static uint8_t s_latch;
static bool s_latch_valid;
static bool s_need_hard;
static uint8_t s_suppress;
static bool s_failback_active;
static uint8_t s_failback_backup_index;
static bool s_session_on_backup;
static uint32_t s_session_gen;
static int64_t s_probe_healthy_since_us;
static int64_t s_next_probe_us;
static bool s_failback_armed;
static int64_t s_failback_at_us;
static int64_t s_last_refresh_us;
static int64_t s_daily_earliest_us;
static bool s_refresh_full_cycle;
static bool s_restart_mqtt;
static bool s_persist;
static bool s_running;
static bool s_boot_pending;
static bool s_logged_literal;
static bool s_no_token_logged;

static char s_body[REFRESH_BODY_MAX];

static void fo_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void fo_unlock(void)
{
    if (s_lock) {
        (void)xSemaphoreGive(s_lock);
    }
}

static void fo_notify(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void iotmer_mqtt_failover_drop_dns_cache(void)
{
    dns_clear_cache();
}

static bool host_is_hostname(const char *host)
{
    if (!host || host[0] == '\0') {
        return false;
    }
    size_t n = strnlen(host, IOTMER_MQTT_HOST_MAX);
    if (n == 0 || n >= IOTMER_MQTT_HOST_MAX) {
        return false;
    }
    bool ipv4 = true;
    for (size_t i = 0; i < n; i++) {
        char c = host[i];
        if (c == ':') {
            return false;
        }
        if (!((c >= '0' && c <= '9') || c == '.')) {
            ipv4 = false;
        }
    }
    return !ipv4;
}

static bool copy_hostname(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0 || !host_is_hostname(src)) {
        return false;
    }
    size_t n = strnlen(src, dst_len - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

static void force_cstr(char *s, size_t len)
{
    if (len == 0) {
        return;
    }
    s[len - 1] = '\0';
}

static void policy_defaults(mqtt_route_blob_t *b)
{
    b->min_fail_window_sec = FLOOR_FAIL_WINDOW_SEC;
    b->same_host_attempts = FLOOR_SAME_HOST_ATTEMPTS;
    b->failback_healthy_sec = FLOOR_FAILBACK_HEALTH_SEC;
    b->probe_interval_sec = DEFAULT_PROBE_INTERVAL_SEC;
    b->failback_jitter_sec = DEFAULT_JITTER_SEC;
}

static void policy_clamp(mqtt_route_blob_t *b)
{
    if (b->min_fail_window_sec < FLOOR_FAIL_WINDOW_SEC) {
        ESP_LOGW(TAG, "min_fail_window_sec %" PRIu32 " raised to %" PRIu32,
                 b->min_fail_window_sec, FLOOR_FAIL_WINDOW_SEC);
        b->min_fail_window_sec = FLOOR_FAIL_WINDOW_SEC;
    }
    if (b->failback_healthy_sec < FLOOR_FAILBACK_HEALTH_SEC) {
        ESP_LOGW(TAG, "failback_healthy_sec %" PRIu32 " raised to %" PRIu32,
                 b->failback_healthy_sec, FLOOR_FAILBACK_HEALTH_SEC);
        b->failback_healthy_sec = FLOOR_FAILBACK_HEALTH_SEC;
    }
    if (b->same_host_attempts < FLOOR_SAME_HOST_ATTEMPTS) {
        ESP_LOGW(TAG, "same_host_attempts %" PRIu32 " raised to %" PRIu32,
                 b->same_host_attempts, FLOOR_SAME_HOST_ATTEMPTS);
        b->same_host_attempts = FLOOR_SAME_HOST_ATTEMPTS;
    }
    if (b->probe_interval_sec < PROBE_INTERVAL_MIN_SEC) {
        ESP_LOGW(TAG, "probe_interval_sec %" PRIu32 " raised to %" PRIu32,
                 b->probe_interval_sec, PROBE_INTERVAL_MIN_SEC);
        b->probe_interval_sec = PROBE_INTERVAL_MIN_SEC;
    } else if (b->probe_interval_sec > PROBE_INTERVAL_MAX_SEC) {
        ESP_LOGW(TAG, "probe_interval_sec %" PRIu32 " lowered to %" PRIu32,
                 b->probe_interval_sec, PROBE_INTERVAL_MAX_SEC);
        b->probe_interval_sec = PROBE_INTERVAL_MAX_SEC;
    }
    if (b->failback_jitter_sec > JITTER_CAP_SEC) {
        b->failback_jitter_sec = JITTER_CAP_SEC;
    }
}

static bool json_u32(cJSON *obj, const char *key, uint32_t *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 1000000.0) {
        return false;
    }
    *out = (uint32_t)v->valuedouble;
    return true;
}

static void policy_from_json(cJSON *policy, mqtt_route_blob_t *b)
{
    policy_defaults(b);
    if (cJSON_IsObject(policy)) {
        uint32_t v = 0;
        if (json_u32(policy, "min_fail_window_sec", &v)) {
            b->min_fail_window_sec = v;
        }
        if (json_u32(policy, "same_host_attempts", &v)) {
            b->same_host_attempts = v;
        }
        if (json_u32(policy, "failback_healthy_sec", &v)) {
            b->failback_healthy_sec = v;
        }
        if (json_u32(policy, "probe_interval_sec", &v)) {
            b->probe_interval_sec = v;
        }
        if (json_u32(policy, "failback_jitter_sec", &v)) {
            b->failback_jitter_sec = v;
        }
    }
    policy_clamp(b);
}

static void set_etag(mqtt_route_blob_t *b, const char *etag)
{
    b->etag[0] = '\0';
    if (!etag || etag[0] == '\0') {
        return;
    }
    size_t n = strnlen(etag, IOTMER_MQTT_ETAG_MAX);
    if (n >= IOTMER_MQTT_ETAG_MAX) {
        ESP_LOGW(TAG, "ETag longer than %d — If-None-Match disabled", IOTMER_MQTT_ETAG_MAX - 1);
        return;
    }
    memcpy(b->etag, etag, n);
    b->etag[n] = '\0';
}

static int find_host(const mqtt_route_blob_t *b, const char *host, uint16_t port)
{
    if (!b || !host || host[0] == '\0') {
        return -1;
    }
    uint8_t n = b->count;
    if (n > IOTMER_MQTT_ENDPOINT_MAX) {
        n = IOTMER_MQTT_ENDPOINT_MAX;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (b->ep[i].port == port && strcasecmp(b->ep[i].host, host) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool same_endpoints(const mqtt_route_blob_t *a, const mqtt_route_blob_t *b)
{
    if (!a || !b || a->count != b->count) {
        return false;
    }
    for (uint8_t i = 0; i < a->count && i < IOTMER_MQTT_ENDPOINT_MAX; i++) {
        if (a->ep[i].port != b->ep[i].port || a->ep[i].tls != b->ep[i].tls ||
            a->ep[i].role != b->ep[i].role || strcasecmp(a->ep[i].host, b->ep[i].host) != 0) {
            return false;
        }
    }
    return true;
}

static uint8_t primary_index_of(const mqtt_route_blob_t *b)
{
    uint8_t n = b->count > IOTMER_MQTT_ENDPOINT_MAX ? IOTMER_MQTT_ENDPOINT_MAX : b->count;
    for (uint8_t i = 0; i < n; i++) {
        if (b->ep[i].role == ROLE_PRIMARY) {
            return i;
        }
    }
    return 0;
}

static void sync_legacy_locked(iotmer_creds_t *creds)
{
    if (!creds || !s_dir.from_server_list || s_dir.count == 0) {
        return;
    }
    memcpy(creds->mqtt_host, s_dir.ep[0].host, sizeof(creds->mqtt_host));
    creds->mqtt_host[sizeof(creds->mqtt_host) - 1] = '\0';
    creds->mqtt_port = s_dir.ep[0].port;
    creds->mqtt_tls = s_dir.ep[0].tls ? true : false;
}

static void synth_legacy_locked(const iotmer_creds_t *creds)
{
    char host[IOTMER_MQTT_HOST_MAX];
    uint16_t port = 8883;
    uint8_t tls = 0;
    host[0] = '\0';
    if (creds && creds->mqtt_host[0] != '\0') {
        size_t n = strnlen(creds->mqtt_host, sizeof(host) - 1);
        memcpy(host, creds->mqtt_host, n);
        host[n] = '\0';
        if (creds->mqtt_port > 0 && creds->mqtt_port <= 65535) {
            port = (uint16_t)creds->mqtt_port;
        }
        tls = creds->mqtt_tls ? 1 : 0;
    }
    memset(&s_dir, 0, sizeof(s_dir));
    s_dir.ver = IOTMER_MQTT_DIR_VER;
    policy_defaults(&s_dir);
    policy_clamp(&s_dir);
    if (host[0] == '\0') {
        return;
    }
    memcpy(s_dir.ep[0].host, host, sizeof(s_dir.ep[0].host));
    s_dir.ep[0].port = port;
    s_dir.ep[0].tls = tls;
    s_dir.ep[0].role = ROLE_PRIMARY;
    s_dir.count = 1;
    s_dir.from_server_list = 0;
}

static void sanitize_dir(mqtt_route_blob_t *b)
{
    if (b->count > IOTMER_MQTT_ENDPOINT_MAX) {
        b->count = IOTMER_MQTT_ENDPOINT_MAX;
    }
    force_cstr(b->etag, sizeof(b->etag));
    force_cstr(b->last_good_host, sizeof(b->last_good_host));
    for (uint8_t i = 0; i < IOTMER_MQTT_ENDPOINT_MAX; i++) {
        force_cstr(b->ep[i].host, sizeof(b->ep[i].host));
    }
    policy_clamp(b);
    b->ver = IOTMER_MQTT_DIR_VER;
}

/* Caller holds the failover lock and, when creds != NULL, the creds lock. */
static void commit_locked(iotmer_creds_t *creds, const mqtt_route_blob_t *neu)
{
    bool session = s_running && s_client && s_client->connected;
    int stay = -1;
    if (session && s_dir.last_good_valid) {
        stay = find_host(neu, s_dir.last_good_host, s_dir.last_good_port);
    }
    if (session && stay < 0 && s_index < s_dir.count) {
        stay = find_host(neu, s_dir.ep[s_index].host, s_dir.ep[s_index].port);
    }

    s_dir = *neu;
    sanitize_dir(&s_dir);

    if (session && stay >= 0) {
        s_index = (uint8_t)stay;
        s_session_on_backup = (s_dir.ep[s_index].role == ROLE_BACKUP);
        if (!s_session_on_backup) {
            s_probe_healthy_since_us = 0;
            s_failback_armed = false;
            s_failback_active = false;
        }
        s_restart_mqtt = false;
        ESP_LOGI(TAG, "endpoint list updated — session stays on %s", s_dir.ep[s_index].host);
    } else if (session) {
        s_index = 0;
        s_attempt_count = 0;
        s_window_start_us = 0;
        s_session_on_backup = false;
        s_probe_healthy_since_us = 0;
        s_failback_armed = false;
        s_failback_active = false;
        s_restart_mqtt = true;
        s_session_gen++;
        ESP_LOGW(TAG, "connected host left the endpoint list — next session uses %s",
                 s_dir.count ? s_dir.ep[0].host : "(none)");
    } else {
        s_index = 0;
        s_restart_mqtt = false;
    }
    sync_legacy_locked(creds);
    s_persist = true;
}

static bool parse_one_endpoint(cJSON *item, mqtt_ep_t *ep, int index)
{
    memset(ep, 0, sizeof(*ep));
    cJSON *host = cJSON_GetObjectItemCaseSensitive(item, "host");
    if (!cJSON_IsString(host)) {
        host = cJSON_GetObjectItemCaseSensitive(item, "hostname");
    }
    if (!cJSON_IsString(host)) {
        host = cJSON_GetObjectItemCaseSensitive(item, "mqtt_host");
    }
    if (!cJSON_IsString(host) || !copy_hostname(ep->host, sizeof(ep->host), host->valuestring)) {
        return false;
    }

    ep->tls = 1;
    cJSON *tls = cJSON_GetObjectItemCaseSensitive(item, "tls");
    if (!cJSON_IsBool(tls)) {
        tls = cJSON_GetObjectItemCaseSensitive(item, "mqtt_tls");
    }
    if (cJSON_IsBool(tls)) {
        ep->tls = cJSON_IsTrue(tls) ? 1 : 0;
    }

    ep->port = ep->tls ? 8883 : 1883;
    cJSON *port = cJSON_GetObjectItemCaseSensitive(item, "port");
    if (!cJSON_IsNumber(port)) {
        port = cJSON_GetObjectItemCaseSensitive(item, "mqtt_port");
    }
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535) {
        ep->port = (uint16_t)port->valueint;
    }

    ep->role = (index == 0) ? ROLE_PRIMARY : ROLE_BACKUP;
    cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
    if (cJSON_IsString(role) && role->valuestring) {
        if (strcasecmp(role->valuestring, "primary") == 0) {
            ep->role = ROLE_PRIMARY;
        } else if (strcasecmp(role->valuestring, "backup") == 0) {
            ep->role = ROLE_BACKUP;
        }
    }
    return true;
}

static cJSON *find_endpoints(cJSON *root, cJSON **policy_out)
{
    *policy_out = NULL;
    if (!cJSON_IsObject(root)) {
        return NULL;
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "mqtt_endpoints");
    cJSON *pol = cJSON_GetObjectItemCaseSensitive(root, "policy");
    if (!cJSON_IsArray(arr)) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsObject(data)) {
            arr = cJSON_GetObjectItemCaseSensitive(data, "mqtt_endpoints");
            if (!cJSON_IsObject(pol)) {
                pol = cJSON_GetObjectItemCaseSensitive(data, "policy");
            }
        }
    }
    if (!cJSON_IsArray(arr)) {
        arr = NULL;
    }
    if (!cJSON_IsObject(pol)) {
        pol = NULL;
    }
    *policy_out = pol;
    return arr;
}

static bool fill_from_array(cJSON *arr, cJSON *policy, mqtt_route_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ver = IOTMER_MQTT_DIR_VER;
    out->from_server_list = 1;
    policy_from_json(policy, out);

    int skipped = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) {
            skipped++;
            continue;
        }
        if (out->count >= IOTMER_MQTT_ENDPOINT_MAX) {
            skipped++;
            continue;
        }
        mqtt_ep_t ep;
        if (!parse_one_endpoint(item, &ep, (int)out->count)) {
            skipped++;
            continue;
        }
        out->ep[out->count] = ep;
        out->count++;
    }
    if (skipped > 0) {
        ESP_LOGW(TAG, "mqtt_endpoints: skipped %d entry (hostname required, max %d)",
                 skipped, IOTMER_MQTT_ENDPOINT_MAX);
    }
    return out->count > 0;
}

static void merge_last_good_locked(mqtt_route_blob_t *neu)
{
    if (!s_dir.last_good_valid) {
        neu->last_good_valid = 0;
        neu->last_good_host[0] = '\0';
        return;
    }
    if (find_host(neu, s_dir.last_good_host, s_dir.last_good_port) < 0) {
        neu->last_good_valid = 0;
        neu->last_good_host[0] = '\0';
        neu->last_good_port = 0;
        return;
    }
    neu->last_good_valid = 1;
    neu->last_good_port = s_dir.last_good_port;
    memcpy(neu->last_good_host, s_dir.last_good_host, sizeof(neu->last_good_host));
    force_cstr(neu->last_good_host, sizeof(neu->last_good_host));
}

void iotmer_mqtt_directory_ingest(struct cJSON *root, iotmer_creds_t *creds)
{
    cJSON *policy = NULL;
    cJSON *arr = find_endpoints((cJSON *)root, &policy);

    if (!arr) {
        iotmer_creds_lock();
        fo_lock();
        if (s_dir.from_server_list && s_dir.count > 0) {
            sync_legacy_locked(creds);
            fo_unlock();
            iotmer_creds_unlock();
            ESP_LOGI(TAG, "provision omitted mqtt_endpoints — keeping stored list (%u)",
                     s_dir.count);
            return;
        }
        synth_legacy_locked(creds);
        s_persist = true;
        fo_unlock();
        iotmer_creds_unlock();
        return;
    }

    mqtt_route_blob_t *neu = calloc(1, sizeof(*neu));
    if (!neu) {
        ESP_LOGE(TAG, "mqtt_endpoints: no heap for route");
        return;
    }
    if (!fill_from_array(arr, policy, neu)) {
        ESP_LOGW(TAG, "mqtt_endpoints: no hostname entries — list unchanged");
        free(neu);
        return;
    }

    iotmer_creds_lock();
    fo_lock();
    merge_last_good_locked(neu);
    if (same_endpoints(&s_dir, neu)) {
        set_etag(neu, s_dir.etag);
    } else {
        neu->etag[0] = '\0';
    }
    uint8_t stored = neu->count;
    commit_locked(creds, neu);
    fo_unlock();
    iotmer_creds_unlock();
    free(neu);
    ESP_LOGI(TAG, "mqtt_endpoints stored (%u)", stored);
}

void iotmer_mqtt_directory_nvs_read(nvs_handle_t handle, iotmer_creds_t *creds)
{
    mqtt_route_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(handle, IOTMER_MQTT_NVS_KEY, &blob, &len);

    iotmer_creds_lock();
    fo_lock();
    if (err == ESP_OK && len == sizeof(blob) && blob.ver == IOTMER_MQTT_DIR_VER && blob.count > 0 &&
        blob.count <= IOTMER_MQTT_ENDPOINT_MAX) {
        s_dir = blob;
        sanitize_dir(&s_dir);
        sync_legacy_locked(creds);
        ESP_LOGI(TAG, "MQTT route loaded (%u endpoint%s, server_list=%s)",
                 s_dir.count, s_dir.count == 1 ? "" : "s",
                 s_dir.from_server_list ? "yes" : "no");
    } else {
        synth_legacy_locked(creds);
    }
    fo_unlock();
    iotmer_creds_unlock();
}

esp_err_t iotmer_mqtt_directory_nvs_write(nvs_handle_t handle)
{
    mqtt_route_blob_t snap;
    fo_lock();
    snap = s_dir;
    s_persist = false;
    fo_unlock();
    snap.ver = IOTMER_MQTT_DIR_VER;
    return nvs_set_blob(handle, IOTMER_MQTT_NVS_KEY, &snap, sizeof(snap));
}

static esp_err_t persist_snapshot(void)
{
    mqtt_route_blob_t snap;
    fo_lock();
    if (!s_persist) {
        fo_unlock();
        return ESP_OK;
    }
    snap = s_dir;
    s_persist = false;
    fo_unlock();

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(CONFIG_IOTMER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        fo_lock();
        s_persist = true;
        fo_unlock();
        return err;
    }
    snap.ver = IOTMER_MQTT_DIR_VER;
    err = nvs_set_blob(h, IOTMER_MQTT_NVS_KEY, &snap, sizeof(snap));
    if (err == ESP_OK && snap.from_server_list && snap.count > 0) {
        err = nvs_set_str(h, "mqtt_host", snap.ep[0].host);
        if (err == ESP_OK) {
            err = nvs_set_i32(h, "mqtt_port", snap.ep[0].port);
        }
        if (err == ESP_OK) {
            err = nvs_set_i8(h, "mqtt_tls", snap.ep[0].tls ? 1 : 0);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT route NVS save failed: %s", esp_err_to_name(err));
        fo_lock();
        s_persist = true;
        fo_unlock();
    }
    return err;
}

void iotmer_mqtt_failover_select_unlocked(const iotmer_client_t *client,
                                          char *host, size_t host_len,
                                          int *port, bool *tls,
                                          uint8_t *index, const char **role)
{
    if (host && host_len > 0) {
        host[0] = '\0';
    }
    if (port) {
        *port = 8883;
    }
    if (tls) {
        *tls = client && client->cfg.tls;
    }
    if (index) {
        *index = 0;
    }
    if (role) {
        *role = "legacy";
    }
    if (!client || !host || host_len == 0) {
        return;
    }

    fo_lock();
    const mqtt_ep_t *ep = NULL;
    bool server = s_dir.from_server_list && s_dir.count > 0;
    if (s_dir.count > 0) {
        if (s_index >= s_dir.count) {
            s_index = 0;
        }
        ep = &s_dir.ep[s_index];
        if (index) {
            *index = s_index;
        }
    }
    if (ep && ep->host[0] != '\0') {
        size_t n = strnlen(ep->host, host_len - 1);
        memcpy(host, ep->host, n);
        host[n] = '\0';
        if (port) {
            *port = ep->port ? ep->port : 8883;
        }
        if (tls) {
            *tls = server ? (ep->tls != 0) : client->cfg.tls;
        }
        if (role) {
            if (!server) {
                *role = "legacy";
            } else if (ep->role == ROLE_BACKUP) {
                *role = "backup";
            } else {
                *role = "primary";
            }
        }
        if (!server && !host_is_hostname(host) && !s_logged_literal) {
            s_logged_literal = true;
            ESP_LOGW(TAG, "legacy mqtt_host is not a hostname — reconnects still drop the DNS cache");
        }
    } else if (client->creds.mqtt_host[0] != '\0') {
        size_t n = strnlen(client->creds.mqtt_host, host_len - 1);
        memcpy(host, client->creds.mqtt_host, n);
        host[n] = '\0';
        if (port) {
            *port = client->creds.mqtt_port ? client->creds.mqtt_port : client->cfg.broker_port;
        }
    }
    fo_unlock();
}

bool iotmer_mqtt_failover_uses_tls(const iotmer_client_t *client)
{
    char host[IOTMER_MQTT_HOST_MAX];
    int port = 0;
    bool tls = true;
    uint8_t index = 0;
    const char *role = NULL;
    iotmer_creds_lock();
    iotmer_mqtt_failover_select_unlocked(client, host, sizeof(host), &port, &tls, &index, &role);
    iotmer_creds_unlock();
    return tls;
}

static void reset_probe_window_locked(void)
{
    s_probe_healthy_since_us = 0;
    s_failback_armed = false;
    s_failback_at_us = 0;
}

void iotmer_mqtt_failover_prepare(iotmer_client_t *client)
{
    iotmer_creds_lock();
    fo_lock();
    s_client = client;
    s_index = 0;
    s_attempt_count = 0;
    s_window_start_us = 0;
    s_latch = LATCH_NONE;
    s_latch_valid = false;
    s_need_hard = false;
    s_suppress = 0;
    s_failback_active = false;
    s_failback_backup_index = 0;
    s_session_on_backup = false;
    s_session_gen++;
    reset_probe_window_locked();
    s_refresh_full_cycle = false;
    s_restart_mqtt = false;
    if (s_dir.count == 0 && client) {
        synth_legacy_locked(&client->creds);
    }
    fo_unlock();
    iotmer_creds_unlock();
}

void iotmer_mqtt_failover_on_connack(iotmer_client_t *client)
{
    if (!client) {
        return;
    }
    fo_lock();
    s_attempt_count = 0;
    s_window_start_us = 0;
    s_latch_valid = false;
    s_failback_active = false;
    reset_probe_window_locked();
    s_session_gen++;

    const char *host = NULL;
    uint16_t port = 0;
    uint8_t role = ROLE_PRIMARY;
    if (s_index < s_dir.count) {
        host = s_dir.ep[s_index].host;
        port = s_dir.ep[s_index].port;
        role = s_dir.ep[s_index].role;
    }
    if (host && host[0] != '\0') {
        bool changed = !s_dir.last_good_valid || s_dir.last_good_port != port ||
                       strcasecmp(s_dir.last_good_host, host) != 0;
        if (changed) {
            size_t n = strnlen(host, sizeof(s_dir.last_good_host) - 1);
            memcpy(s_dir.last_good_host, host, n);
            s_dir.last_good_host[n] = '\0';
            s_dir.last_good_port = port;
            s_dir.last_good_valid = 1;
            s_persist = true;
            ESP_LOGI(TAG, "last_good %s:%u", s_dir.last_good_host, (unsigned)port);
        }
    }
    if (s_dir.from_server_list && role == ROLE_BACKUP) {
        s_session_on_backup = true;
        s_next_probe_us = esp_timer_get_time() +
                          (int64_t)s_dir.probe_interval_sec * 1000000LL;
    } else {
        s_session_on_backup = false;
    }
    fo_unlock();
    fo_notify();
}

void iotmer_mqtt_failover_on_error(const esp_mqtt_error_codes_t *err)
{
    if (!err || !s_running) {
        return;
    }
    uint8_t latch = LATCH_NONE;
    if (err->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
        return;
    }
    if (err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        int rc = (int)err->connect_return_code;
        if (rc == MQTT_CONNECTION_REFUSE_PROTOCOL || rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
            rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
            latch = (rc == MQTT_CONNECTION_REFUSE_PROTOCOL) ? LATCH_PROTOCOL : LATCH_AUTH;
        } else if (rc == MQTT_CONNECTION_REFUSE_ID_REJECTED) {
            latch = LATCH_ID_REJECTED;
            ESP_LOGW(TAG, "MQTT identifier rejected — previous session may still be online");
        } else {
            latch = LATCH_TRANSPORT;
        }
    } else {
        latch = LATCH_TRANSPORT;
    }
    fo_lock();
    s_latch = latch;
    s_latch_valid = true;
    fo_unlock();
}

static void leave_host_locked(iotmer_mqtt_fo_step_t *step, bool *wake)
{
    uint8_t count = s_dir.count;
    if (count > IOTMER_MQTT_ENDPOINT_MAX) {
        count = IOTMER_MQTT_ENDPOINT_MAX;
    }
    bool at_end = (count <= 1) || (s_index + 1 >= count);
    s_attempt_count = 0;
    s_window_start_us = 0;
    reset_probe_window_locked();
    s_session_on_backup = false;
    if (at_end && s_task) {
        s_refresh_full_cycle = true;
        step->arm_timer = false;
        *wake = true;
        ESP_LOGW(TAG, "every MQTT endpoint failed the window — refreshing the list");
        return;
    }
    if (!at_end) {
        const char *from = s_dir.ep[s_index].host;
        s_index++;
        s_need_hard = true;
        ESP_LOGW(TAG, "leaving %s — next %s (%u/%u)",
                 from, s_dir.ep[s_index].host, (unsigned)(s_index + 1), (unsigned)count);
    } else {
        s_index = 0;
        s_need_hard = true;
        ESP_LOGW(TAG, "endpoint list exhausted — retrying %s",
                 count ? s_dir.ep[0].host : "(none)");
    }
    step->arm_timer = true;
}

void iotmer_mqtt_failover_on_disconnect(iotmer_client_t *client, iotmer_mqtt_fo_step_t *step)
{
    if (!step) {
        return;
    }
    uint32_t normal = 5000;
    if (client && client->cfg.reconnect_delay_ms > 0) {
        normal = (uint32_t)client->cfg.reconnect_delay_ms;
    }
    step->wait_ms = normal;
    step->arm_timer = true;

    bool wake = false;
    bool wifi = iotmer_wifi_sta_has_ip();

    fo_lock();
    s_session_gen++;
    s_session_on_backup = false;
    uint8_t latch = s_latch_valid ? s_latch : LATCH_TRANSPORT;
    s_latch_valid = false;
    if (!s_running || !client) {
        step->arm_timer = false;
        fo_unlock();
        return;
    }

    if (!wifi) {
        ESP_LOGI(TAG, "Wi-Fi down — MQTT broker index unchanged");
        fo_unlock();
        return;
    }

    if (s_failback_active) {
        s_failback_active = false;
        s_index = s_failback_backup_index;
        if (s_dir.count > 0 && s_index >= s_dir.count) {
            s_index = 0;
        }
        s_attempt_count = 0;
        s_window_start_us = 0;
        reset_probe_window_locked();
        s_need_hard = true;
        step->arm_timer = true;
        ESP_LOGW(TAG, "primary CONNECT failed — reconnecting to the same backup");
        fo_unlock();
        return;
    }

    if (latch == LATCH_AUTH || latch == LATCH_PROTOCOL) {
        if (latch == LATCH_AUTH && client->mqtt_auth_backoff_ms != 0) {
            step->wait_ms = client->mqtt_auth_backoff_ms;
        }
        ESP_LOGI(TAG, "CONNACK does not change region (%s)",
                 latch == LATCH_AUTH ? "not authorized" : "protocol");
        fo_unlock();
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s_window_start_us == 0) {
        s_window_start_us = now;
        s_attempt_count = 1;
    } else if (s_attempt_count < UINT32_MAX) {
        s_attempt_count++;
    }
    int64_t span_s = (now - s_window_start_us) / 1000000LL;
    if (span_s < 0) {
        span_s = 0;
    }
    const char *host = (s_index < s_dir.count) ? s_dir.ep[s_index].host : "?";
    if (s_attempt_count >= s_dir.same_host_attempts &&
        span_s >= (int64_t)s_dir.min_fail_window_sec) {
        leave_host_locked(step, &wake);
    } else {
        ESP_LOGI(TAG,
                 "staying on %s (attempt %" PRIu32 "/%" PRIu32 ", span %lld/%" PRIu32 " s)",
                 host, s_attempt_count, s_dir.same_host_attempts,
                 (long long)span_s, s_dir.min_fail_window_sec);
    }
    fo_unlock();
    if (wake) {
        fo_notify();
    }
}

void iotmer_mqtt_failover_suppress_disconnect(void)
{
    fo_lock();
    if (s_suppress < 255) {
        s_suppress++;
    }
    fo_unlock();
}

bool iotmer_mqtt_failover_consume_suppress(void)
{
    fo_lock();
    bool yes = s_suppress > 0;
    if (yes) {
        s_suppress--;
        s_latch_valid = false;
        s_session_gen++;
    }
    fo_unlock();
    return yes;
}

bool iotmer_mqtt_failover_consume_need_hard(void)
{
    fo_lock();
    bool yes = s_need_hard;
    s_need_hard = false;
    fo_unlock();
    return yes;
}

void iotmer_mqtt_failover_clear_need_hard(void)
{
    fo_lock();
    s_need_hard = false;
    fo_unlock();
}

static bool probe_tcp(const char *host, int port)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    iotmer_mqtt_failover_drop_dns_cache();
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        return false;
    }
    bool ok = false;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int nb = 1;
        (void)ioctl(fd, FIONBIO, &nb);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv = {
                .tv_sec = PROBE_TIMEOUT_MS / 1000,
                .tv_usec = (PROBE_TIMEOUT_MS % 1000) * 1000,
            };
            if (select(fd + 1, NULL, &wset, NULL, &tv) > 0) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                    ok = true;
                }
            }
        }
        shutdown(fd, SHUT_RDWR);
        closesocket(fd);
        if (ok) {
            break;
        }
    }
    freeaddrinfo(res);
    return ok;
}

static bool probe_tls(const char *host, int port)
{
    esp_tls_cfg_t cfg = { 0 };
    cfg.timeout_ms = PROBE_TIMEOUT_MS;
    const char *pem = iotmer_tls_get_ca_cert_pem();
    if (pem && pem[0] != '\0') {
        cfg.cacert_buf = (const unsigned char *)pem;
        cfg.cacert_bytes = (unsigned int)strlen(pem) + 1U;
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    iotmer_mqtt_failover_drop_dns_cache();
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        return false;
    }
    int rc = esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, tls);
    esp_tls_conn_destroy(tls);
    return rc == 1;
}

static void note_probe_locked(bool ok, int64_t now)
{
    if (!ok) {
        reset_probe_window_locked();
        ESP_LOGW(TAG, "primary probe failed — failback window reset");
        return;
    }
    if (s_probe_healthy_since_us == 0) {
        s_probe_healthy_since_us = now;
    }
    int64_t span_s = (now - s_probe_healthy_since_us) / 1000000LL;
    if (!s_failback_armed && span_s >= (int64_t)s_dir.failback_healthy_sec) {
        uint32_t jitter = s_dir.failback_jitter_sec;
        uint32_t delay = (jitter == 0) ? 0 : (esp_random() % (jitter + 1U));
        s_failback_armed = true;
        s_failback_at_us = now + (int64_t)delay * 1000000LL;
        ESP_LOGI(TAG, "primary healthy for %lld s — failback in %" PRIu32 " s",
                 (long long)span_s, delay);
    } else {
        ESP_LOGD(TAG, "primary probe ok (%lld/%" PRIu32 " s)",
                 (long long)span_s, s_dir.failback_healthy_sec);
    }
}

static void maybe_probe(void)
{
    char host[IOTMER_MQTT_HOST_MAX];
    int port = 8883;
    bool use_tls = true;
    uint32_t gen = 0;
    bool run = false;

    fo_lock();
    int64_t now = esp_timer_get_time();
    if (s_running && s_session_on_backup && !s_failback_active && s_client &&
        s_client->connected && !s_client->tls_hook_held && now >= s_next_probe_us &&
        s_dir.count > 0) {
        uint8_t pi = primary_index_of(&s_dir);
        if (copy_hostname(host, sizeof(host), s_dir.ep[pi].host)) {
            port = s_dir.ep[pi].port ? s_dir.ep[pi].port : 8883;
            use_tls = s_dir.ep[pi].tls != 0;
            gen = s_session_gen;
            s_next_probe_us = now + (int64_t)s_dir.probe_interval_sec * 1000000LL;
            run = true;
        }
    }
    bool switch_now = false;
    if (s_running && s_failback_armed && s_session_on_backup && !s_failback_active &&
        s_client && s_client->connected && now >= s_failback_at_us) {
        uint8_t pi = primary_index_of(&s_dir);
        if (pi != s_index) {
            s_failback_backup_index = s_index;
            s_index = pi;
            s_failback_active = true;
            s_failback_armed = false;
            s_session_on_backup = false;
            s_attempt_count = 0;
            s_window_start_us = 0;
            s_session_gen++;
            switch_now = true;
        } else {
            s_failback_armed = false;
        }
    }
    fo_unlock();

        if (switch_now && s_running && s_client) {
        ESP_LOGI(TAG, "failback: disconnect backup, CONNECT primary");
        (void)iotmer_reconnect_hard(s_client);
        return;
    }
    if (!run || !s_client) {
        return;
    }

    iotmer_tls_hook_acquire(s_client, use_tls);
    bool ok = use_tls ? probe_tls(host, port) : probe_tcp(host, port);
    iotmer_tls_hook_release(s_client);

    bool switch_after = false;
    fo_lock();
    if (gen == s_session_gen && s_session_on_backup && !s_failback_active) {
        note_probe_locked(ok, esp_timer_get_time());
        int64_t after = esp_timer_get_time();
        if (s_failback_armed && after >= s_failback_at_us) {
            uint8_t pi = primary_index_of(&s_dir);
            if (pi != s_index) {
                s_failback_backup_index = s_index;
                s_index = pi;
                s_failback_active = true;
                s_failback_armed = false;
                s_session_on_backup = false;
                s_attempt_count = 0;
                s_window_start_us = 0;
                s_session_gen++;
                switch_after = true;
            } else {
                s_failback_armed = false;
            }
        }
    }
    fo_unlock();
    if (switch_after && s_running && s_client) {
        ESP_LOGI(TAG, "failback: disconnect backup, CONNECT primary");
        (void)iotmer_reconnect_hard(s_client);
    }
}

typedef struct {
    char etag[IOTMER_MQTT_ETAG_MAX];
} refresh_hdr_t;

static esp_err_t refresh_http_event(esp_http_client_event_t *evt)
{
    if (!evt || evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data) {
        return ESP_OK;
    }
    if (!evt->header_key || !evt->header_value) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "ETag") != 0) {
        return ESP_OK;
    }
    refresh_hdr_t *hdr = (refresh_hdr_t *)evt->user_data;
    size_t n = strnlen(evt->header_value, sizeof(hdr->etag) - 1);
    if (strnlen(evt->header_value, sizeof(hdr->etag)) >= sizeof(hdr->etag)) {
        hdr->etag[0] = '\0';
        return ESP_OK;
    }
    memcpy(hdr->etag, evt->header_value, n);
    hdr->etag[n] = '\0';
    return ESP_OK;
}

static bool copy_token(char *dst, size_t dst_len)
{
    dst[0] = '\0';
    if (!s_client || dst_len == 0) {
        return false;
    }
    iotmer_creds_lock();
    size_t n = strnlen(s_client->creds.device_http_token, dst_len - 1);
    if (s_client->creds.device_http_token[0] != '\0' &&
        strnlen(s_client->creds.device_http_token, dst_len) < dst_len) {
        memcpy(dst, s_client->creds.device_http_token, n);
        dst[n] = '\0';
    }
    iotmer_creds_unlock();
    return dst[0] != '\0';
}

static esp_err_t refresh_list(int reason)
{
    char token[384];
    if (!copy_token(token, sizeof(token))) {
        if (!s_no_token_logged) {
            s_no_token_logged = true;
            ESP_LOGW(TAG, "mqtt-endpoints skipped — device_http_token missing");
        }
        if (reason == REFRESH_DAILY || reason == REFRESH_BOOT) {
            s_daily_earliest_us = esp_timer_get_time() + HOUR_US;
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_no_token_logged = false;

    char url[512];
    int n = snprintf(url, sizeof(url), "%s/devices/auth/mqtt-endpoints",
                     CONFIG_IOTMER_PROVISION_API_URL);
    if (n < 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_NO_MEM;
    }

    char etag[IOTMER_MQTT_ETAG_MAX];
    etag[0] = '\0';
    fo_lock();
    if (s_dir.etag[0] != '\0') {
        memcpy(etag, s_dir.etag, sizeof(etag));
        force_cstr(etag, sizeof(etag));
    }
    fo_unlock();

    refresh_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_IOTMER_PROVISION_TIMEOUT_MS,
        .buffer_size = 2048,
        .event_handler = refresh_http_event,
        .user_data = &hdr,
    };
    iotmer_tls_apply_http_client_config(&cfg);

    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        return ESP_ERR_NO_MEM;
    }

    char auth[448];
    int an = snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_err_t err = ESP_OK;
    if (an < 0 || an >= (int)sizeof(auth)) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    err = esp_http_client_set_header(http, "Authorization", auth);
    if (err != ESP_OK) {
        goto out;
    }
    if (etag[0] != '\0') {
        err = esp_http_client_set_header(http, "If-None-Match", etag);
        if (err != ESP_OK) {
            goto out;
        }
    }

    const char *why = (reason == REFRESH_BOOT) ? "boot" :
                      (reason == REFRESH_CYCLE) ? "all-endpoints-failed" : "daily";
    ESP_LOGI(TAG, "GET mqtt-endpoints (%s)", why);
    s_last_refresh_us = esp_timer_get_time();
    s_daily_earliest_us = s_last_refresh_us + DAY_US;

    err = esp_http_client_open(http, 0);
    if (err != ESP_OK) {
        goto out;
    }
    if (esp_http_client_fetch_headers(http) < 0) {
        err = ESP_ERR_HTTP_FETCH_HEADER;
        goto out;
    }
    int status = esp_http_client_get_status_code(http);
    s_body[0] = '\0';
    if (status == 200) {
        int total = 0;
        while (total < (int)sizeof(s_body) - 1) {
            int r = esp_http_client_read(http, s_body + total,
                                         (int)sizeof(s_body) - 1 - total);
            if (r < 0) {
                err = ESP_FAIL;
                goto out;
            }
            if (r == 0) {
                break;
            }
            total += r;
        }
        s_body[total] = '\0';
    }

    if (status == 304) {
        if (hdr.etag[0] != '\0') {
            fo_lock();
            set_etag(&s_dir, hdr.etag);
            s_persist = true;
            fo_unlock();
        }
        ESP_LOGI(TAG, "mqtt-endpoints not modified");
        err = ESP_OK;
        goto out;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "mqtt-endpoints HTTP %d", status);
        err = ESP_FAIL;
        goto out;
    }

    cJSON *root = cJSON_Parse(s_body);
    if (!root) {
        ESP_LOGW(TAG, "mqtt-endpoints JSON parse failed");
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    cJSON *policy = NULL;
    cJSON *arr = find_endpoints(root, &policy);
    mqtt_route_blob_t *neu = calloc(1, sizeof(*neu));
    if (!neu) {
        cJSON_Delete(root);
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    bool filled = false;
    if (arr) {
        filled = fill_from_array(arr, policy, neu);
    } else {
        cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "mqtt_host");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (!cJSON_IsString(host) && cJSON_IsObject(data)) {
            host = cJSON_GetObjectItemCaseSensitive(data, "mqtt_host");
        }
        if (cJSON_IsString(host) && copy_hostname(neu->ep[0].host, sizeof(neu->ep[0].host),
                                                  host->valuestring)) {
            neu->ver = IOTMER_MQTT_DIR_VER;
            neu->count = 1;
            neu->from_server_list = 1;
            neu->ep[0].role = ROLE_PRIMARY;
            neu->ep[0].tls = 1;
            neu->ep[0].port = 8883;
            cJSON *src = cJSON_IsObject(data) ? data : root;
            cJSON *port = cJSON_GetObjectItemCaseSensitive(src, "mqtt_port");
            cJSON *tls = cJSON_GetObjectItemCaseSensitive(src, "mqtt_tls");
            if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535) {
                neu->ep[0].port = (uint16_t)port->valueint;
            }
            if (cJSON_IsBool(tls)) {
                neu->ep[0].tls = cJSON_IsTrue(tls) ? 1 : 0;
            }
            policy_from_json(policy, neu);
            filled = true;
        }
    }
    if (!filled) {
        ESP_LOGW(TAG, "mqtt-endpoints body has no hostname");
        free(neu);
        cJSON_Delete(root);
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    iotmer_creds_lock();
    fo_lock();
    merge_last_good_locked(neu);
    if (hdr.etag[0] != '\0') {
        set_etag(neu, hdr.etag);
    } else if (same_endpoints(&s_dir, neu)) {
        set_etag(neu, s_dir.etag);
    }
    commit_locked(s_client ? &s_client->creds : NULL, neu);
    fo_unlock();
    iotmer_creds_unlock();
    ESP_LOGI(TAG, "mqtt-endpoints applied (%u)", neu->count);
    free(neu);
    cJSON_Delete(root);
    err = ESP_OK;

out:
    esp_http_client_cleanup(http);
    memset(token, 0, sizeof(token));
    (void)persist_snapshot();
    return err;
}

static uint32_t reconnect_wait_ms(void)
{
    if (s_client && s_client->cfg.reconnect_delay_ms > 0) {
        return (uint32_t)s_client->cfg.reconnect_delay_ms;
    }
    return 5000;
}

static void arm_reconnect(uint32_t wait_ms)
{
    if (!s_running || !s_client || !s_client->reconnect_timer) {
        return;
    }
    (void)esp_timer_stop((esp_timer_handle_t)s_client->reconnect_timer);
    (void)esp_timer_start_once((esp_timer_handle_t)s_client->reconnect_timer,
                               (uint64_t)wait_ms * 1000ULL);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_boot_pending) {
            s_boot_pending = false;
            (void)refresh_list(REFRESH_BOOT);
            if (s_boot_sem) {
                (void)xSemaphoreGive(s_boot_sem);
            }
            continue;
        }
        if (!s_running) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        bool cycle = false;
        bool restart = false;
        fo_lock();
        cycle = s_refresh_full_cycle;
        if (cycle) {
            s_refresh_full_cycle = false;
        }
        fo_unlock();

        if (cycle) {
            (void)refresh_list(REFRESH_CYCLE);
            fo_lock();
            s_index = 0;
            s_attempt_count = 0;
            s_window_start_us = 0;
            reset_probe_window_locked();
            s_failback_active = false;
            s_session_on_backup = false;
            s_need_hard = (s_client && s_client->mqtt != NULL);
            s_restart_mqtt = false;
            fo_unlock();
            arm_reconnect(reconnect_wait_ms());
            continue;
        }

        (void)persist_snapshot();
        maybe_probe();

        fo_lock();
        restart = s_restart_mqtt;
        if (restart) {
            s_restart_mqtt = false;
        }
        fo_unlock();
        if (restart && s_running && s_client && s_client->mqtt) {
            (void)iotmer_reconnect_hard(s_client);
            continue;
        }

        if (s_client && s_client->connected) {
            int64_t now = esp_timer_get_time();
            bool due = now >= s_daily_earliest_us &&
                       (s_last_refresh_us == 0 || now - s_last_refresh_us >= DAY_US);
            if (due) {
                (void)refresh_list(REFRESH_DAILY);
                fo_lock();
                restart = s_restart_mqtt;
                if (restart) {
                    s_restart_mqtt = false;
                }
                fo_unlock();
                if (restart && s_running && s_client && s_client->mqtt) {
                    (void)iotmer_reconnect_hard(s_client);
                }
            }
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

esp_err_t iotmer_mqtt_failover_start(iotmer_client_t *client)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_boot_sem) {
        s_boot_sem = xSemaphoreCreateBinary();
        if (!s_boot_sem) {
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(s_boot_sem, 0) == pdTRUE) {
    }

    s_client = client;
    s_running = true;
    s_boot_pending = true;

    if (!s_task) {
        BaseType_t ok = xTaskCreate(worker, "iotmer_fo", TASK_STACK, NULL, TASK_PRIO, &s_task);
        if (ok != pdPASS) {
            s_task = NULL;
            s_boot_pending = false;
            s_running = false;
            ESP_LOGE(TAG, "failover task create failed");
            return ESP_ERR_NO_MEM;
        }
    } else {
        fo_notify();
    }

    uint32_t wait_ms = (uint32_t)CONFIG_IOTMER_PROVISION_TIMEOUT_MS + 8000U;
    if (xSemaphoreTake(s_boot_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "boot mqtt-endpoints still running — connecting with the stored list");
    }
    return ESP_OK;
}

void iotmer_mqtt_failover_stop(void)
{
    fo_lock();
    s_running = false;
    s_session_on_backup = false;
    s_failback_armed = false;
    s_refresh_full_cycle = false;
    s_restart_mqtt = false;
    s_session_gen++;
    fo_unlock();
    fo_notify();
}
