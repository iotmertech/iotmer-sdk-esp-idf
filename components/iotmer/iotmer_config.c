/*
 * iotmer_config.c — MQTT Config Protocol (device side).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mqtt_client.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "zlib.h"

#include "iotmer_config.h"
#include "iotmer_internal.h"

#define TAG "iotmer_config"

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

static size_t half_cap(const iotmer_config_ctx_t *ctx)
{
    return ctx->cap / 2U;
}

static bool topic_suffix_is(const char *topic, const char *suffix)
{
    if (!topic || !suffix) {
        return false;
    }
    size_t lt = strlen(topic);
    size_t ls = strlen(suffix);
    return (lt >= ls) && (strcmp(topic + (lt - ls), suffix) == 0);
}

static void emit_fail(iotmer_config_event_cb_t cb, void *user_ctx, const char *rid,
                      const char *msg)
{
    if (!cb) {
        return;
    }
    iotmer_config_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = IOTMER_CONFIG_EV_FAIL;
    if (rid) {
        strncpy(ev.rid, rid, sizeof(ev.rid) - 1U);
    }
    strncpy(ev.u.fail.message, msg, sizeof(ev.u.fail.message) - 1U);
    cb(user_ctx, &ev);
}

static void gen_uuid_v4(char out[IOTMER_CONFIG_RID_LEN])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0FU) | 0x40U); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3FU) | 0x80U); /* RFC variant */
    snprintf(out, IOTMER_CONFIG_RID_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
}

static void sha256_hex_lower(const uint8_t hash[32], char out[IOTMER_CONFIG_SHA_HEX_LEN])
{
    static const char *xd = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = xd[(hash[i] >> 4) & 0x0FU];
        out[i * 2 + 1] = xd[hash[i] & 0x0FU];
    }
    out[64] = '\0';
}

static bool sha256_hex_equal_ci(const char *a, const char *b)
{
    if (!a || !b || strlen(a) != 64U || strlen(b) != 64U) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static esp_err_t sha256_bytes(const uint8_t *data, size_t len, char out_hex[IOTMER_CONFIG_SHA_HEX_LEN])
{
    if (!data || !out_hex) {
        return ESP_ERR_INVALID_ARG;
    }
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) {
        return ESP_FAIL;
    }
    unsigned char hash[32];
    if (mbedtls_md(md, data, len, hash) != 0) {
        return ESP_FAIL;
    }
    sha256_hex_lower(hash, out_hex);
    return ESP_OK;
}

static esp_err_t publish_suffix_json(iotmer_client_t *client, const char *suffix, const char *json)
{
    if (!client || !suffix || !json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!client->mqtt || !client->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (client->creds.workspace_slug[0] == '\0' || client->creds.device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[256];
    esp_err_t err = iotmer_topics_build_publish(topic, sizeof(topic), client->creds.workspace_slug,
                                                client->creds.device_key, suffix);
    if (err != ESP_OK) {
        return err;
    }
    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)client->mqtt, topic, json, 0,
                                         1 /* QoS1 */, 0 /* retain */);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "publish %s msg_id=%d", topic, msg_id);
    return ESP_OK;
}

static void fmt_iso8601_utc_z(char *out, size_t out_len)
{
    if (!out || out_len < 21U) {
        return;
    }
    time_t t = time(NULL);
    if (t <= (time_t)1577836800) { /* 2020-01-01 — clock likely unset */
        snprintf(out, out_len, "1970-01-01T00:00:00Z");
        return;
    }
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* -------------------------------------------------------------------------- */
/* Chunked config/resp (gzip or identity)                                    */
/* -------------------------------------------------------------------------- */

static bool content_type_is_application_json(const char *s)
{
    static const char want[] = "application/json";
    if (!s) {
        return false;
    }
    size_t i = 0;
    for (; want[i] != '\0'; i++) {
        char c = s[i];
        if (c == '\0') {
            return false;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != want[i]) {
            return false;
        }
    }
    return s[i] == '\0';
}

/* -------------------------------------------------------------------------- */
/* gzip + base64                                                             */
/* -------------------------------------------------------------------------- */

static esp_err_t gunzip_to_upper(iotmer_config_ctx_t *ctx, size_t gzip_len, size_t *json_len_out)
{
    if (!ctx || !json_len_out) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t h = half_cap(ctx);
    if (gzip_len == 0U || gzip_len > h || gzip_len > ctx->cap - h) {
        return ESP_ERR_INVALID_ARG;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in  = ctx->buf;
    strm.avail_in = (uInt)gzip_len;
    strm.next_out = ctx->buf + h;
    strm.avail_out = (uInt)(ctx->cap - h);

    int zr = inflateInit2(&strm, 15 + 16);
    if (zr != Z_OK) {
        return ESP_FAIL;
    }
    zr = inflate(&strm, Z_FINISH);
    (void)inflateEnd(&strm);
    if (zr != Z_STREAM_END) {
        ESP_LOGW(TAG, "gzip inflate failed zr=%d", zr);
        return ESP_FAIL;
    }
    *json_len_out = (size_t)strm.total_out;
    return ESP_OK;
}

static esp_err_t b64_decode_append(iotmer_config_ctx_t *ctx, const char *b64, size_t b64_len)
{
    if (!ctx || !b64 || b64_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t h = half_cap(ctx);
    if (ctx->gzip_len >= h) {
        return ESP_ERR_NO_MEM;
    }
    size_t olen = 0;
    int br = mbedtls_base64_decode(ctx->buf + ctx->gzip_len, h - ctx->gzip_len, &olen,
                                   (const unsigned char *)b64, b64_len);
    if (br != 0) {
        ESP_LOGW(TAG, "base64 decode failed rc=%d", br);
        return ESP_FAIL;
    }
    ctx->gzip_len += olen;
    if (ctx->gzip_len > h) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Chunk bitmap                                                               */
/* -------------------------------------------------------------------------- */

static bool chunk_mark(iotmer_config_ctx_t *ctx, uint32_t idx)
{
    if (idx >= IOTMER_CONFIG_MAX_CHUNKS) {
        return false;
    }
    uint32_t word = idx / 32U;
    uint32_t bit  = idx % 32U;
    uint32_t mask = (1U << bit);
    if (ctx->chunk_bmap[word] & mask) {
        return false; /* duplicate */
    }
    ctx->chunk_bmap[word] |= mask;
    return true;
}

static bool chunks_complete(const iotmer_config_ctx_t *ctx)
{
    if (ctx->total_chunks == 0U || ctx->total_chunks > IOTMER_CONFIG_MAX_CHUNKS) {
        return false;
    }
    for (uint32_t i = 0; i < ctx->total_chunks; i++) {
        uint32_t word = i / 32U;
        uint32_t bit  = i % 32U;
        if ((ctx->chunk_bmap[word] & (1U << bit)) == 0U) {
            return false;
        }
    }
    return true;
}

static void transfer_reset(iotmer_config_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->transfer_active    = false;
    ctx->chunk_meta_init    = false;
    ctx->resp_chunk_is_gzip = false;
    ctx->total_chunks       = 0;
    ctx->gzip_len           = 0;
    ctx->pending_rid[0]     = '\0';
    memset(ctx->chunk_bmap, 0, sizeof(ctx->chunk_bmap));
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void iotmer_config_ctx_init(iotmer_config_ctx_t *ctx, uint8_t *buf, size_t cap)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->buf = buf;
    ctx->cap = cap;
}

void iotmer_config_set_have(iotmer_config_ctx_t *ctx, uint32_t version, const char *sha256_hex)
{
    if (!ctx) {
        return;
    }
    ctx->have_valid   = true;
    ctx->have_version = version;
    memset(ctx->have_sha_hex, 0, sizeof(ctx->have_sha_hex));
    if (sha256_hex) {
        strncpy(ctx->have_sha_hex, sha256_hex, sizeof(ctx->have_sha_hex) - 1U);
    }
}

void iotmer_config_clear_have(iotmer_config_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    ctx->have_valid = false;
    memset(ctx->have_sha_hex, 0, sizeof(ctx->have_sha_hex));
    ctx->have_version = 0;
}

esp_err_t iotmer_config_request(iotmer_config_ctx_t *ctx, iotmer_client_t *client,
                                uint32_t chunk_bytes, uint32_t max_total_bytes,
                                char rid_out[IOTMER_CONFIG_RID_LEN])
{
    if (!ctx || !client || !rid_out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->buf || ctx->cap < 4096U || (ctx->cap % 2U) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->transfer_active) {
        return ESP_ERR_INVALID_STATE;
    }

    char rid[IOTMER_CONFIG_RID_LEN];
    gen_uuid_v4(rid);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "rid", rid);

    if (ctx->have_valid) {
        cJSON *have = cJSON_CreateObject();
        if (have) {
            cJSON_AddNumberToObject(have, "version", (double)ctx->have_version);
            cJSON_AddStringToObject(have, "sha256", ctx->have_sha_hex);
            cJSON_AddItemToObject(root, "have", have);
        }
    }

    cJSON *want = cJSON_CreateObject();
    if (!want) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (chunk_bytes > 0U) {
        cJSON_AddNumberToObject(want, "chunk_bytes", (double)chunk_bytes);
    }
    if (max_total_bytes > 0U) {
        cJSON_AddNumberToObject(want, "max_total_bytes", (double)max_total_bytes);
    }
    cJSON *enc = cJSON_CreateArray();
    if (enc) {
        cJSON_AddItemToArray(enc, cJSON_CreateString("gzip"));
        cJSON_AddItemToArray(enc, cJSON_CreateString("identity"));
        cJSON_AddItemToObject(want, "accept_encoding", enc);
    }
    cJSON_AddItemToObject(root, "want", want);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = publish_suffix_json(client, "config/get", printed);
    cJSON_free(printed);

    if (err != ESP_OK) {
        return err;
    }

    strncpy(ctx->pending_rid, rid, sizeof(ctx->pending_rid) - 1U);
    ctx->pending_rid[sizeof(ctx->pending_rid) - 1U] = '\0';
    strncpy(rid_out, rid, IOTMER_CONFIG_RID_LEN - 1U);
    rid_out[IOTMER_CONFIG_RID_LEN - 1U] = '\0';

    ctx->transfer_active    = true;
    ctx->chunk_meta_init    = false;
    ctx->gzip_len           = 0;
    ctx->total_chunks       = 0;
    memset(ctx->chunk_bmap, 0, sizeof(ctx->chunk_bmap));
    return ESP_OK;
}

esp_err_t iotmer_config_publish_status(iotmer_client_t *client, const char *rid, bool applied,
                                       uint32_t version, const char *sha256_hex,
                                       const char *err_code, const char *err_msg)
{
    if (!client || !rid || !sha256_hex) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "rid", rid);
    cJSON_AddBoolToObject(root, "applied", applied ? 1 : 0);
    cJSON_AddNumberToObject(root, "version", (double)version);
    cJSON_AddStringToObject(root, "sha256", sha256_hex);

    char ts[32];
    fmt_iso8601_utc_z(ts, sizeof(ts));
    cJSON_AddStringToObject(root, "applied_at", ts);

    if (applied) {
        cJSON_AddNullToObject(root, "error");
    } else {
        cJSON *err = cJSON_CreateObject();
        if (err) {
            if (err_code) {
                cJSON_AddStringToObject(err, "code", err_code);
            } else {
                cJSON_AddStringToObject(err, "code", "CONFIG_APPLY_FAILED");
            }
            cJSON_AddStringToObject(err, "message", err_msg ? err_msg : "");
            cJSON_AddItemToObject(root, "error", err);
        }
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = publish_suffix_json(client, "config/status", printed);
    cJSON_free(printed);
    return e;
}

static esp_err_t handle_meta(iotmer_config_ctx_t *ctx, iotmer_client_t *client,
                             const char *payload, int payload_len, iotmer_config_event_cb_t cb,
                             void *user_ctx)
{
    (void)client;
    if (!ctx || !payload || payload_len <= 0 || !cb) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t h = half_cap(ctx);
    if ((size_t)payload_len + 1U > ctx->cap - h) {
        emit_fail(cb, user_ctx, NULL, "meta: payload too large for parse buffer");
        return ESP_ERR_NO_MEM;
    }
    memcpy(ctx->buf + h, payload, (size_t)payload_len);
    ctx->buf[h + (size_t)payload_len] = '\0';

    cJSON *root = cJSON_Parse((char *)(ctx->buf + h));
    if (!root) {
        emit_fail(cb, user_ctx, NULL, "meta: JSON parse failed");
        return ESP_FAIL;
    }
    cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, "bytes");
    if (!cJSON_IsNumber(jv) || !cJSON_IsString(js) || js->valuestring == NULL) {
        cJSON_Delete(root);
        emit_fail(cb, user_ctx, NULL, "meta: missing version/sha256");
        return ESP_FAIL;
    }
    uint32_t ver = (uint32_t)jv->valuedouble;
    const char *sha = js->valuestring;
    size_t bytes_hint = 0;
    if (cJSON_IsNumber(jb)) {
        bytes_hint = (size_t)jb->valuedouble;
    }
    cJSON_Delete(root);

    iotmer_config_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = IOTMER_CONFIG_EV_META;
    ev.u.meta.version = ver;
    strncpy(ev.u.meta.sha256_hex, sha, sizeof(ev.u.meta.sha256_hex) - 1U);
    ev.u.meta.bytes_hint = bytes_hint;
    cb(user_ctx, &ev);
    return ESP_OK;
}

static esp_err_t handle_resp_chunked(iotmer_config_ctx_t *ctx, cJSON *root,
                                     iotmer_config_event_cb_t cb, void *user_ctx)
{
    cJSON *jrid = cJSON_GetObjectItemCaseSensitive(root, "rid");
    cJSON *jok  = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *jsha = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    cJSON *jenc = cJSON_GetObjectItemCaseSensitive(root, "encoding");
    cJSON *jct  = cJSON_GetObjectItemCaseSensitive(root, "content_type");
    cJSON *jcb  = cJSON_GetObjectItemCaseSensitive(root, "chunk_bytes");
    cJSON *jidx = cJSON_GetObjectItemCaseSensitive(root, "chunk_index");
    cJSON *jtot = cJSON_GetObjectItemCaseSensitive(root, "total_chunks");
    cJSON *jb64 = cJSON_GetObjectItemCaseSensitive(root, "data_b64");

    if (!cJSON_IsString(jrid) || jrid->valuestring == NULL || !cJSON_IsBool(jok) ||
        !cJSON_IsNumber(jver) || !cJSON_IsString(jsha) || jsha->valuestring == NULL ||
        !cJSON_IsString(jenc) || jenc->valuestring == NULL || !cJSON_IsNumber(jidx) ||
        !cJSON_IsNumber(jtot) || !cJSON_IsString(jb64) || jb64->valuestring == NULL ||
        !cJSON_IsString(jct) || jct->valuestring == NULL || !cJSON_IsNumber(jcb)) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): missing fields");
        transfer_reset(ctx);
        return ESP_FAIL;
    }
    (void)jcb;

    if (!content_type_is_application_json(jct->valuestring)) {
        emit_fail(cb, user_ctx, ctx->pending_rid,
                  "resp(chunked): content_type must be application/json");
        transfer_reset(ctx);
        return ESP_FAIL;
    }

    if (strcmp(jrid->valuestring, ctx->pending_rid) != 0) {
        /* Not for our active transfer */
        return ESP_OK;
    }
    if (!cJSON_IsTrue(jok)) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): ok!=true");
        transfer_reset(ctx);
        return ESP_FAIL;
    }

    const char *enc = jenc->valuestring;
    bool path_gzip;
    if (strstr(enc, "gzip") != NULL) {
        path_gzip = true;
    } else if (strstr(enc, "identity") != NULL) {
        path_gzip = false;
    } else {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): encoding unsupported");
        transfer_reset(ctx);
        return ESP_FAIL;
    }

    uint32_t ver    = (uint32_t)jver->valuedouble;
    const char *sha = jsha->valuestring;
    uint32_t idx    = (uint32_t)jidx->valuedouble;
    uint32_t tot    = (uint32_t)jtot->valuedouble;

    if (tot == 0U || tot > IOTMER_CONFIG_MAX_CHUNKS) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): total_chunks invalid");
        transfer_reset(ctx);
        return ESP_FAIL;
    }

    if (!ctx->chunk_meta_init) {
        ctx->chunk_meta_init     = true;
        ctx->resp_chunk_is_gzip  = path_gzip;
        ctx->resp_version        = ver;
        strncpy(ctx->resp_sha_hex, sha, sizeof(ctx->resp_sha_hex) - 1U);
        ctx->resp_sha_hex[sizeof(ctx->resp_sha_hex) - 1U] = '\0';
        ctx->total_chunks = tot;
        ctx->gzip_len     = 0;
        memset(ctx->chunk_bmap, 0, sizeof(ctx->chunk_bmap));
    } else {
        if (ctx->resp_version != ver || strcmp(ctx->resp_sha_hex, sha) != 0 ||
            ctx->total_chunks != tot || ctx->resp_chunk_is_gzip != path_gzip) {
            emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): inconsistent metadata");
            transfer_reset(ctx);
            return ESP_FAIL;
        }
    }

    bool is_new_chunk = chunk_mark(ctx, idx);
    if (!is_new_chunk) {
        ESP_LOGW(TAG, "duplicate chunk_index=%u (ignored)", (unsigned)idx);
    } else {
        esp_err_t eb =
            b64_decode_append(ctx, jb64->valuestring, strlen(jb64->valuestring));
        if (eb != ESP_OK) {
            emit_fail(cb, user_ctx, ctx->pending_rid,
                       "resp(chunked): base64/decode append failed");
            transfer_reset(ctx);
            return eb;
        }
    }

    if (!chunks_complete(ctx)) {
        return ESP_OK;
    }

    const size_t h = half_cap(ctx);
    size_t         json_len = 0;
    const uint8_t *sha_in  = NULL;

    if (ctx->resp_chunk_is_gzip) {
        esp_err_t gi = gunzip_to_upper(ctx, ctx->gzip_len, &json_len);
        if (gi != ESP_OK) {
            emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): gunzip failed");
            transfer_reset(ctx);
            return gi;
        }
        sha_in = ctx->buf + h;
    } else {
        json_len = ctx->gzip_len;
        if (json_len == 0U || json_len > h) {
            emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): identity payload invalid");
            transfer_reset(ctx);
            return ESP_ERR_NO_MEM;
        }
        if (json_len + 1U > ctx->cap - h) {
            emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): json too large for buffer");
            transfer_reset(ctx);
            return ESP_ERR_NO_MEM;
        }
        sha_in = ctx->buf;
    }

    char calc[IOTMER_CONFIG_SHA_HEX_LEN];
    esp_err_t se = sha256_bytes(sha_in, json_len, calc);
    if (se != ESP_OK) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): sha256 failed");
        transfer_reset(ctx);
        return se;
    }
    if (!sha256_hex_equal_ci(calc, ctx->resp_sha_hex)) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp(chunked): sha256 mismatch");
        transfer_reset(ctx);
        return ESP_ERR_INVALID_CRC;
    }

    if (!ctx->resp_chunk_is_gzip) {
        memmove(ctx->buf + h, ctx->buf, json_len);
    }
    ctx->buf[h + json_len] = '\0';

    if (cb) {
        iotmer_config_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = IOTMER_CONFIG_EV_CONFIG_JSON;
        strncpy(ev.rid, ctx->pending_rid, sizeof(ev.rid) - 1U);
        ev.u.config.version = ctx->resp_version;
        strncpy(ev.u.config.sha256_hex, ctx->resp_sha_hex, sizeof(ev.u.config.sha256_hex) - 1U);
        ev.u.config.json_utf8 = ctx->buf + h;
        ev.u.config.json_len  = json_len;
        cb(user_ctx, &ev);
    }

    transfer_reset(ctx);
    return ESP_OK;
}

static esp_err_t handle_resp_error(iotmer_config_ctx_t *ctx, cJSON *root,
                                   iotmer_config_event_cb_t cb, void *user_ctx)
{
    cJSON *jrid = cJSON_GetObjectItemCaseSensitive(root, "rid");
    cJSON *jok  = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *jer  = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsString(jrid) || jrid->valuestring == NULL || !cJSON_IsBool(jok) ||
        !cJSON_IsObject(jer)) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp(err): missing fields");
        transfer_reset(ctx);
        return ESP_FAIL;
    }
    if (strcmp(jrid->valuestring, ctx->pending_rid) != 0) {
        return ESP_OK;
    }
    if (cJSON_IsTrue(jok)) {
        return ESP_OK;
    }

    cJSON *jc = cJSON_GetObjectItemCaseSensitive(jer, "code");
    cJSON *jm = cJSON_GetObjectItemCaseSensitive(jer, "message");
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(jer, "retryable");
    const char *code = (cJSON_IsString(jc) && jc->valuestring) ? jc->valuestring : "ERROR";
    const char *msg  = (cJSON_IsString(jm) && jm->valuestring) ? jm->valuestring : "";
    bool retryable   = cJSON_IsBool(jr) ? cJSON_IsTrue(jr) : false;

    if (cb) {
        iotmer_config_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = IOTMER_CONFIG_EV_RESP_ERROR;
        strncpy(ev.rid, jrid->valuestring, sizeof(ev.rid) - 1U);
        strncpy(ev.u.resp_err.code, code, sizeof(ev.u.resp_err.code) - 1U);
        strncpy(ev.u.resp_err.message, msg, sizeof(ev.u.resp_err.message) - 1U);
        ev.u.resp_err.retryable = retryable;
        cb(user_ctx, &ev);
    }
    transfer_reset(ctx);
    return ESP_OK;
}

static esp_err_t handle_resp(iotmer_config_ctx_t *ctx, const char *payload, int payload_len,
                             iotmer_config_event_cb_t cb, void *user_ctx)
{
    if (!ctx->transfer_active || ctx->pending_rid[0] == '\0') {
        /* Not expecting a response */
        return ESP_OK;
    }
    const size_t h = half_cap(ctx);
    if ((size_t)payload_len + 1U > ctx->cap - h) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp: payload too large for parse buffer");
        transfer_reset(ctx);
        return ESP_ERR_NO_MEM;
    }
    memcpy(ctx->buf + h, payload, (size_t)payload_len);
    ctx->buf[h + (size_t)payload_len] = '\0';

    cJSON *root = cJSON_Parse((char *)(ctx->buf + h));
    if (!root) {
        emit_fail(cb, user_ctx, ctx->pending_rid, "resp: JSON parse failed");
        transfer_reset(ctx);
        return ESP_FAIL;
    }

    cJSON *jok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (cJSON_IsBool(jok) && cJSON_IsFalse(jok)) {
        esp_err_t e = handle_resp_error(ctx, root, cb, user_ctx);
        cJSON_Delete(root);
        return e;
    }

    cJSON *jidx = cJSON_GetObjectItemCaseSensitive(root, "chunk_index");
    if (cJSON_IsNumber(jidx)) {
        esp_err_t e = handle_resp_chunked(ctx, root, cb, user_ctx);
        cJSON_Delete(root);
        return e;
    }

    emit_fail(cb, user_ctx, ctx->pending_rid,
              "resp: missing chunk_index (chunked data_b64 required)");
    transfer_reset(ctx);
    cJSON_Delete(root);
    return ESP_FAIL;
}

esp_err_t iotmer_config_on_mqtt(iotmer_config_ctx_t *ctx, iotmer_client_t *client,
                                const char *topic, const char *payload, int payload_len,
                                iotmer_config_event_cb_t cb, void *user_ctx)
{
    if (!ctx || !client || !topic || !cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!payload || payload_len < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->buf || ctx->cap < 4096U || (ctx->cap % 2U) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!iotmer_topics_match_config(topic, client->creds.workspace_slug, client->creds.device_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (topic_suffix_is(topic, "config/meta")) {
        return handle_meta(ctx, client, payload, payload_len, cb, user_ctx);
    }
    if (topic_suffix_is(topic, "config/resp")) {
        return handle_resp(ctx, payload, payload_len, cb, user_ctx);
    }
    (void)payload_len;
    return ESP_OK;
}
