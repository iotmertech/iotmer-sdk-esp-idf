/*
 * IOTMER BLE Wi-Fi provisioning — NimBLE GATT server (protocol v1).
 */

#include "sdkconfig.h"

#if !CONFIG_IOTMER_BLE_WIFI_PROV

#include "iotmer_ble_wifi_prov.h"

#include "esp_err.h"

esp_err_t iotmer_ble_wifi_prov_init(const iotmer_ble_wifi_prov_cfg_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void iotmer_ble_wifi_prov_deinit(void) {}

esp_err_t iotmer_ble_wifi_prov_start(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t iotmer_ble_wifi_prov_stop(void) { return ESP_OK; }

#else

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "store/config/ble_store_config.h"

#include "iotmer_ble_wifi_prov.h"
#include "iotmer_ble_wifi_prov_priv.h"
#include "iotmer_wifi.h"

void ble_hs_lock(void);
void ble_hs_unlock(void);
void ble_store_config_init(void);

/* ---- UUIDs (canonical strings in public header) ---- */

static const ble_uuid128_t s_uuid_svc = BLE_UUID128_INIT(
    0xee, 0xd6, 0x14, 0x1d, 0x01, 0x00, 0x00, 0x40, 0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);

static const ble_uuid128_t s_uuid_ctrl = BLE_UUID128_INIT(
    0xee, 0xd6, 0x14, 0x1d, 0x01, 0x01, 0x00, 0x40, 0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);

static const ble_uuid128_t s_uuid_data = BLE_UUID128_INIT(
    0xee, 0xd6, 0x14, 0x1d, 0x02, 0x02, 0x00, 0x40, 0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);

static const ble_uuid128_t s_uuid_evt = BLE_UUID128_INIT(
    0xee, 0xd6, 0x14, 0x1d, 0x03, 0x03, 0x00, 0x40, 0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);

static uint16_t s_chr_evt_val_handle;

static QueueHandle_t    s_q;
static TaskHandle_t     s_worker;
static SemaphoreHandle_t s_worker_exit_sem;
static esp_timer_handle_t s_timer_idle;
static esp_timer_handle_t s_timer_sess;

static iotmer_ble_wifi_prov_cfg_t s_cfg;
static bool                       s_inited;
static bool                       s_nimble_inited;
static bool                       s_host_task_started;
static bool                       s_running;
static uint8_t                    s_own_addr_type;
static uint16_t                   s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

typedef enum {
    RX_NONE = 0,
    RX_SSID,
    RX_PASS,
} rx_phase_t;

static rx_phase_t s_rx;
static char       s_ssid[33];
static char       s_pass[66];
static size_t     s_ssid_len;
static size_t     s_pass_len;

static void reset_rx(void)
{
    s_rx        = RX_NONE;
    s_ssid[0]   = '\0';
    s_pass[0]   = '\0';
    s_ssid_len  = 0;
    s_pass_len  = 0;
}

static uint32_t eff_adv_ms(void)
{
    if (s_cfg.adv_timeout_ms != 0u) {
        return s_cfg.adv_timeout_ms;
    }
    return (uint32_t)CONFIG_IOTMER_BLE_ADV_TIMEOUT_MS;
}

static uint32_t eff_idle_ms(void)
{
    if (s_cfg.conn_idle_ms != 0u) {
        return s_cfg.conn_idle_ms;
    }
    return (uint32_t)CONFIG_IOTMER_BLE_CONN_IDLE_MS;
}

static uint32_t eff_sess_ms(void)
{
    if (s_cfg.session_max_ms != 0u) {
        return s_cfg.session_max_ms;
    }
    return (uint32_t)CONFIG_IOTMER_BLE_SESSION_MAX_MS;
}

static void emit_state(iotmer_ble_wifi_prov_state_t st)
{
    if (s_cfg.on_state) {
        s_cfg.on_state(s_cfg.user_ctx, st);
    }
}

static void emit_result(esp_err_t e, iotmer_ble_wifi_prov_err_t d)
{
    if (s_cfg.on_result) {
        s_cfg.on_result(s_cfg.user_ctx, e, d);
    }
}

static int notify_frame(uint16_t conn, uint8_t evt, uint16_t app_err, const void *payload, uint16_t pay_len)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || conn == 0xffffu) {
        return 0;
    }

    uint8_t buf[6 + 128];
    if (pay_len > sizeof(buf) - 6u) {
        pay_len = (uint16_t)(sizeof(buf) - 6u);
    }
    buf[0] = IOTMER_BLE_WIFI_PROV_PROTO_VER;
    buf[1] = evt;
    buf[2] = (uint8_t)(app_err & 0xffu);
    buf[3] = (uint8_t)((app_err >> 8) & 0xffu);
    buf[4] = (uint8_t)(pay_len & 0xffu);
    buf[5] = (uint8_t)((pay_len >> 8) & 0xffu);
    if (pay_len && payload) {
        memcpy(buf + 6, payload, pay_len);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)(6u + pay_len));
    if (!om) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    ble_hs_lock();
    int rc = ble_gatts_notify_custom(conn, s_chr_evt_val_handle, om);
    ble_hs_unlock();
    if (rc != 0) {
        ESP_LOGW(IOTMER_BLE_PROV_TAG, "notify failed rc=%d", rc);
    }
    return 0;
}

static void stop_idle_timer(void)
{
    if (s_timer_idle) {
        (void)esp_timer_stop(s_timer_idle);
    }
}

static void stop_sess_timer(void)
{
    if (s_timer_sess) {
        (void)esp_timer_stop(s_timer_sess);
    }
}

static void arm_idle_timer(void)
{
    if (!s_timer_idle || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    uint64_t us = (uint64_t)eff_idle_ms() * 1000ULL;
    if (us == 0ULL) {
        return;
    }
    (void)esp_timer_stop(s_timer_idle);
    (void)esp_timer_start_once(s_timer_idle, us);
}

static void arm_sess_timer(void)
{
    if (!s_timer_sess || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    uint64_t us = (uint64_t)eff_sess_ms() * 1000ULL;
    if (us == 0ULL) {
        return;
    }
    (void)esp_timer_stop(s_timer_sess);
    (void)esp_timer_start_once(s_timer_sess, us);
}

static void timer_idle_cb(void *arg)
{
    (void)arg;
    iotmer_ble_q_msg_t m = {.kind = IOTMER_BLE_Q_TIMER_IDLE};
    if (s_q) {
        (void)xQueueSend(s_q, &m, 0);
    }
}

static void timer_sess_cb(void *arg)
{
    (void)arg;
    iotmer_ble_q_msg_t m = {.kind = IOTMER_BLE_Q_TIMER_SESS};
    if (s_q) {
        (void)xQueueSend(s_q, &m, 0);
    }
}

static int build_gap_name(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    const char *pfx = CONFIG_IOTMER_BLE_ADV_NAME_PREFIX;
    if (!pfx) {
        pfx = "IOTMER-";
    }
    int n = snprintf(out, out_len, "%s%02X%02X%02X", pfx, mac[3], mac[4], mac[5]);
    if (n <= 0 || (size_t)n >= out_len) {
        out[out_len - 1] = '\0';
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    iotmer_ble_q_msg_t m = {0};
    m.kind = IOTMER_BLE_Q_GAP;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        m.conn_handle = event->connect.conn_handle;
        m.gap_evt     = (uint8_t)BLE_GAP_EVENT_CONNECT;
        m.gap_status  = event->connect.status;
        if (s_q) {
            (void)xQueueSend(s_q, &m, 0);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        m.conn_handle = event->disconnect.conn.conn_handle;
        m.gap_evt     = (uint8_t)BLE_GAP_EVENT_DISCONNECT;
        m.gap_status  = event->disconnect.reason;
        if (s_q) {
            (void)xQueueSend(s_q, &m, 0);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        m.gap_evt    = (uint8_t)BLE_GAP_EVENT_ADV_COMPLETE;
        m.gap_status = event->adv_complete.reason;
        if (s_q) {
            (void)xQueueSend(s_q, &m, 0);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(IOTMER_BLE_PROV_TAG, "MTU update mtu=%d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static int start_adv_locked(void)
{
    /* Legacy advertising data is max 31 bytes per PDU. Flags + complete name + 128-bit UUID
     * does not fit; keep UUID in the advertising PDU and put the GAP name in scan response. */
    struct ble_hs_adv_fields adv;
    struct ble_hs_adv_fields rsp;
    memset(&adv, 0, sizeof(adv));
    memset(&rsp, 0, sizeof(rsp));

    adv.flags                   = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128                = &s_uuid_svc;
    adv.num_uuids128            = 1;
    adv.uuids128_is_complete    = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(IOTMER_BLE_PROV_TAG, "adv_set_fields rc=%d", rc);
        return rc;
    }

    const char *name = ble_svc_gap_device_name();
    rsp.name          = (uint8_t *)name;
    rsp.name_len      = (uint8_t)strlen(name);
    if (rsp.name_len > 0) {
        rsp.name_is_complete = 1;
        rc                   = ble_gap_adv_rsp_set_fields(&rsp);
        if (rc != 0) {
            ESP_LOGE(IOTMER_BLE_PROV_TAG, "adv_rsp_set_fields rc=%d", rc);
            return rc;
        }
    }

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int32_t duration_ms = BLE_HS_FOREVER;
    uint32_t adv_ms     = eff_adv_ms();
    if (adv_ms > 0u) {
        duration_ms = (int32_t)adv_ms;
    }

    rc = ble_gap_adv_start(s_own_addr_type, NULL, duration_ms, &advp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(IOTMER_BLE_PROV_TAG, "adv_start rc=%d", rc);
        return rc;
    }

    emit_state(IOTMER_BLE_WIFI_PROV_STATE_ADVERTISING);
    return 0;
}

static void gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (ctxt->op == BLE_GATT_REGISTER_OP_CHR) {
        if (ble_uuid_cmp(ctxt->chr.chr_def->uuid, &s_uuid_evt.u) == 0) {
            s_chr_evt_val_handle = ctxt->chr.val_handle;
        }
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    const ble_uuid_t *u = ctxt->chr->uuid;
    if (ble_uuid_cmp(u, &s_uuid_ctrl.u) == 0) {
        iotmer_ble_q_msg_t m = {.kind = IOTMER_BLE_Q_GATT_CTRL, .conn_handle = conn_handle};
        uint16_t om_len      = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len > sizeof(m.data)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (om_len > 0) {
            uint16_t copied = 0;
            int      rcf    = ble_hs_mbuf_to_flat(ctxt->om, m.data, sizeof(m.data), &copied);
            if (rcf != 0 || copied != om_len) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            m.len = copied;
        } else {
            m.len = 0;
        }
        if (s_q && xQueueSend(s_q, &m, pdMS_TO_TICKS(50)) != pdTRUE) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }

    if (ble_uuid_cmp(u, &s_uuid_data.u) == 0) {
        iotmer_ble_q_msg_t m = {.kind = IOTMER_BLE_Q_GATT_DATA, .conn_handle = conn_handle};
        uint16_t om_len      = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len > sizeof(m.data)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (om_len > 0) {
            uint16_t copied = 0;
            int      rcf    = ble_hs_mbuf_to_flat(ctxt->om, m.data, sizeof(m.data), &copied);
            if (rcf != 0 || copied != om_len) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            m.len = copied;
        } else {
            m.len = 0;
        }
        if (s_q && xQueueSend(s_q, &m, pdMS_TO_TICKS(50)) != pdTRUE) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }

    if (ble_uuid_cmp(u, &s_uuid_evt.u) == 0) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type           = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid           = &s_uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid      = &s_uuid_ctrl.u,
             .access_cb = gatt_access,
             .flags     = BLE_GATT_CHR_F_WRITE},
            {.uuid      = &s_uuid_data.u,
             .access_cb = gatt_access,
             .flags     = BLE_GATT_CHR_F_WRITE},
            {.uuid      = &s_uuid_evt.u,
             .access_cb = gatt_access,
             .flags     = BLE_GATT_CHR_F_NOTIFY},
            {0},
        },
    },
    {0},
};

static int gatt_svcs_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return rc;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        return rc;
    }
    return 0;
}

static void on_reset(int reason)
{
    ESP_LOGE(IOTMER_BLE_PROV_TAG, "NimBLE reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(IOTMER_BLE_PROV_TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(IOTMER_BLE_PROV_TAG, "infer_auto rc=%d", rc);
        return;
    }
    ESP_LOGI(IOTMER_BLE_PROV_TAG, "NimBLE synced");
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void worker_task(void *arg)
{
    (void)arg;
    iotmer_ble_q_msg_t m;

    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (m.kind == IOTMER_BLE_Q_SHUTDOWN) {
            break;
        }

        switch (m.kind) {
        case IOTMER_BLE_Q_TIMER_IDLE:
        case IOTMER_BLE_Q_TIMER_SESS:
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                uint16_t ch = s_conn_handle;
                (void)notify_frame(ch, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                   IOTMER_BLE_WIFI_PROV_ERR_SESSION_TIMEOUT, NULL, 0);
                emit_result(ESP_ERR_TIMEOUT, IOTMER_BLE_WIFI_PROV_ERR_SESSION_TIMEOUT);
                ble_hs_lock();
                (void)ble_gap_terminate(ch, BLE_ERR_REM_USER_CONN_TERM);
                ble_hs_unlock();
            }
            stop_idle_timer();
            stop_sess_timer();
            break;

        case IOTMER_BLE_Q_GAP:
            if (m.gap_evt == BLE_GAP_EVENT_CONNECT) {
                if (m.gap_status == 0) {
                    s_conn_handle = m.conn_handle;
                    reset_rx();
                    emit_state(IOTMER_BLE_WIFI_PROV_STATE_CONNECTED);
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_STATE,
                                       IOTMER_BLE_WIFI_PROV_ERR_NONE, "conn", 4);
                    arm_idle_timer();
                    arm_sess_timer();
                } else if (s_running && CONFIG_IOTMER_BLE_RESTART_ADV_ON_DISCONNECT) {
                    ble_hs_lock();
                    (void)start_adv_locked();
                    ble_hs_unlock();
                }
            } else if (m.gap_evt == BLE_GAP_EVENT_DISCONNECT) {
                stop_idle_timer();
                stop_sess_timer();
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                reset_rx();
                emit_state(IOTMER_BLE_WIFI_PROV_STATE_IDLE);
                if (s_running && CONFIG_IOTMER_BLE_RESTART_ADV_ON_DISCONNECT) {
                    ble_hs_lock();
                    (void)start_adv_locked();
                    ble_hs_unlock();
                }
            } else if (m.gap_evt == BLE_GAP_EVENT_ADV_COMPLETE) {
                /* Stack stopped advertising (e.g. duration elapsed) without a link. */
                if (s_running && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                    emit_state(IOTMER_BLE_WIFI_PROV_STATE_FAILED);
                    emit_result(ESP_ERR_TIMEOUT, IOTMER_BLE_WIFI_PROV_ERR_ADV_TIMEOUT);
                    s_running = false;
                }
            }
            break;

        case IOTMER_BLE_Q_GATT_CTRL: {
            if (m.conn_handle != s_conn_handle) {
                break;
            }
            arm_idle_timer();
            if (m.len < 1u) {
                break;
            }
            uint8_t op = m.data[0];
            switch (op) {
            case IOTMER_BLE_WIFI_PROV_OP_PING:
                (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_PROGRESS,
                                   IOTMER_BLE_WIFI_PROV_ERR_NONE, "pong", 4);
                break;

            case IOTMER_BLE_WIFI_PROV_OP_BEGIN_SSID:
                s_rx       = RX_SSID;
                s_ssid[0]  = '\0';
                s_ssid_len = 0;
                emit_state(IOTMER_BLE_WIFI_PROV_STATE_RECEIVING);
                (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_PROGRESS,
                                   IOTMER_BLE_WIFI_PROV_ERR_NONE, "ssid", 4);
                break;

            case IOTMER_BLE_WIFI_PROV_OP_BEGIN_PASS:
                if (s_ssid_len == 0u) {
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                       IOTMER_BLE_WIFI_PROV_ERR_SSID_EMPTY, NULL, 0);
                    break;
                }
                s_rx       = RX_PASS;
                s_pass[0]  = '\0';
                s_pass_len = 0;
                (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_PROGRESS,
                                   IOTMER_BLE_WIFI_PROV_ERR_NONE, "pass", 4);
                break;

            case IOTMER_BLE_WIFI_PROV_OP_ABORT:
                reset_rx();
                (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                   IOTMER_BLE_WIFI_PROV_ERR_NONE, "abort", 5);
                break;

            case IOTMER_BLE_WIFI_PROV_OP_COMMIT: {
                emit_state(IOTMER_BLE_WIFI_PROV_STATE_COMMITTING);
                if (s_ssid_len == 0u) {
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                       IOTMER_BLE_WIFI_PROV_ERR_SSID_EMPTY, NULL, 0);
                    emit_result(ESP_ERR_INVALID_ARG, IOTMER_BLE_WIFI_PROV_ERR_SSID_EMPTY);
                    emit_state(IOTMER_BLE_WIFI_PROV_STATE_FAILED);
                    break;
                }
                if (s_pass_len == 0u) {
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                       IOTMER_BLE_WIFI_PROV_ERR_PASS_EMPTY, NULL, 0);
                    emit_result(ESP_ERR_INVALID_ARG, IOTMER_BLE_WIFI_PROV_ERR_PASS_EMPTY);
                    emit_state(IOTMER_BLE_WIFI_PROV_STATE_FAILED);
                    break;
                }
                esp_err_t er = iotmer_wifi_set_credentials(s_ssid, s_pass);
                if (er != ESP_OK) {
                    uint8_t pe[4] = {(uint8_t)(er & 0xff), (uint8_t)((er >> 8) & 0xff),
                                   (uint8_t)((er >> 16) & 0xff), (uint8_t)((er >> 24) & 0xff)};
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                       IOTMER_BLE_WIFI_PROV_ERR_NVS, pe, sizeof(pe));
                    emit_result(er, IOTMER_BLE_WIFI_PROV_ERR_NVS);
                    emit_state(IOTMER_BLE_WIFI_PROV_STATE_FAILED);
                } else {
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_DONE,
                                       IOTMER_BLE_WIFI_PROV_ERR_NONE, "ok", 2);
                    emit_result(ESP_OK, IOTMER_BLE_WIFI_PROV_ERR_NONE);
                    emit_state(IOTMER_BLE_WIFI_PROV_STATE_SUCCESS);
                }
                reset_rx();
            } break;

            default:
                (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                   IOTMER_BLE_WIFI_PROV_ERR_BAD_OPCODE, NULL, 0);
                break;
            }
        } break;

        case IOTMER_BLE_Q_GATT_DATA: {
            if (m.conn_handle != s_conn_handle) {
                break;
            }
            arm_idle_timer();
            if (m.len == 0u) {
                break;
            }
            if (s_rx == RX_SSID) {
                if (s_ssid_len + m.len > IOTMER_BLE_WIFI_PROV_SSID_MAX) {
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                       IOTMER_BLE_WIFI_PROV_ERR_SSID_TOO_LONG, NULL, 0);
                    reset_rx();
                    break;
                }
                memcpy(s_ssid + s_ssid_len, m.data, m.len);
                s_ssid_len += m.len;
                s_ssid[s_ssid_len] = '\0';
            } else if (s_rx == RX_PASS) {
                if (s_pass_len + m.len > IOTMER_BLE_WIFI_PROV_PASS_MAX) {
                    (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                       IOTMER_BLE_WIFI_PROV_ERR_PASS_TOO_LONG, NULL, 0);
                    reset_rx();
                    break;
                }
                memcpy(s_pass + s_pass_len, m.data, m.len);
                s_pass_len += m.len;
                s_pass[s_pass_len] = '\0';
            } else {
                (void)notify_frame(s_conn_handle, IOTMER_BLE_WIFI_PROV_EVT_ERROR,
                                   IOTMER_BLE_WIFI_PROV_ERR_INVALID_STATE, NULL, 0);
            }
        } break;

        default:
            break;
        }
    }

    if (s_worker_exit_sem) {
        (void)xSemaphoreGive(s_worker_exit_sem);
    }
    s_worker = NULL;
    vTaskDelete(NULL);
}

esp_err_t iotmer_ble_wifi_prov_init(const iotmer_ble_wifi_prov_cfg_t *cfg)
{
    if (s_inited) {
        return ESP_OK;
    }

    if (!cfg) {
        iotmer_ble_wifi_prov_cfg_t d = IOTMER_BLE_WIFI_PROV_CFG_DEFAULT();
        s_cfg = d;
    } else {
        s_cfg = *cfg;
    }

    s_worker_exit_sem = xSemaphoreCreateBinary();
    if (!s_worker_exit_sem) {
        return ESP_ERR_NO_MEM;
    }

    s_q = xQueueCreate(IOTMER_BLE_Q_DEPTH, sizeof(iotmer_ble_q_msg_t));
    if (!s_q) {
        vSemaphoreDelete(s_worker_exit_sem);
        s_worker_exit_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t targs = {.callback = &timer_idle_cb, .name = "iotmer_ble_idle"};
    if (esp_timer_create(&targs, &s_timer_idle) != ESP_OK) {
        vQueueDelete(s_q);
        s_q = NULL;
        vSemaphoreDelete(s_worker_exit_sem);
        s_worker_exit_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    targs.callback = timer_sess_cb;
    targs.name     = "iotmer_ble_sess";
    if (esp_timer_create(&targs, &s_timer_sess) != ESP_OK) {
        esp_timer_delete(s_timer_idle);
        s_timer_idle = NULL;
        vQueueDelete(s_q);
        s_q = NULL;
        vSemaphoreDelete(s_worker_exit_sem);
        s_worker_exit_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(worker_task, "iotmer_ble_prov", 4096, NULL, 5, &s_worker) != pdPASS) {
        esp_timer_delete(s_timer_sess);
        esp_timer_delete(s_timer_idle);
        s_timer_sess = s_timer_idle = NULL;
        vQueueDelete(s_q);
        s_q = NULL;
        vSemaphoreDelete(s_worker_exit_sem);
        s_worker_exit_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(IOTMER_BLE_PROV_TAG, "nimble_port_init: %s", esp_err_to_name(err));
        goto fail;
    }
    s_nimble_inited = true;

    ble_hs_cfg.reset_cb           = on_reset;
    ble_hs_cfg.sync_cb            = on_sync;
    ble_hs_cfg.gatts_register_cb  = gatt_register_cb;
    ble_hs_cfg.store_status_cb    = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap          = (uint8_t)CONFIG_IOTMER_BLE_SMP_IO_CAP;
    ble_hs_cfg.sm_bonding         = 0;
    ble_hs_cfg.sm_mitm            = 0;
    ble_hs_cfg.sm_sc              = 1;

    int rc = gatt_svcs_init();
    if (rc != 0) {
        ESP_LOGE(IOTMER_BLE_PROV_TAG, "gatt_svcs_init rc=%d", rc);
        err = ESP_FAIL;
        goto fail_nimble;
    }

    char nm[32];
    if (build_gap_name(nm, sizeof(nm)) != ESP_OK) {
        strncpy(nm, "IOTMER-DEV", sizeof(nm) - 1);
    }
    rc = ble_svc_gap_device_name_set(nm);
    if (rc != 0) {
        ESP_LOGE(IOTMER_BLE_PROV_TAG, "device_name_set rc=%d", rc);
        err = ESP_FAIL;
        goto fail_nimble;
    }

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    s_host_task_started = true;

    s_inited = true;
    emit_state(IOTMER_BLE_WIFI_PROV_STATE_IDLE);
    return ESP_OK;

fail_nimble:
    if (s_host_task_started) {
        nimble_port_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        s_host_task_started = false;
    }
    if (s_nimble_inited) {
        nimble_port_deinit();
        s_nimble_inited = false;
    }
fail:
    if (s_worker) {
        vTaskDelete(s_worker);
        s_worker = NULL;
    }
    if (s_timer_sess) {
        esp_timer_delete(s_timer_sess);
        s_timer_sess = NULL;
    }
    if (s_timer_idle) {
        esp_timer_delete(s_timer_idle);
        s_timer_idle = NULL;
    }
    if (s_q) {
        vQueueDelete(s_q);
        s_q = NULL;
    }
    if (s_worker_exit_sem) {
        vSemaphoreDelete(s_worker_exit_sem);
        s_worker_exit_sem = NULL;
    }
    return err;
}

void iotmer_ble_wifi_prov_deinit(void)
{
    if (!s_inited) {
        return;
    }
    (void)iotmer_ble_wifi_prov_stop();

    if (s_q && s_worker_exit_sem) {
        iotmer_ble_q_msg_t m = {.kind = IOTMER_BLE_Q_SHUTDOWN};
        (void)xQueueSend(s_q, &m, pdMS_TO_TICKS(500));
        (void)xSemaphoreTake(s_worker_exit_sem, pdMS_TO_TICKS(2000));
    }

    if (s_host_task_started) {
        nimble_port_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        s_host_task_started = false;
    }
    if (s_nimble_inited) {
        nimble_port_deinit();
        s_nimble_inited = false;
    }

    if (s_timer_idle) {
        esp_timer_delete(s_timer_idle);
        s_timer_idle = NULL;
    }
    if (s_timer_sess) {
        esp_timer_delete(s_timer_sess);
        s_timer_sess = NULL;
    }
    if (s_worker_exit_sem) {
        vSemaphoreDelete(s_worker_exit_sem);
        s_worker_exit_sem = NULL;
    }
    if (s_q) {
        vQueueDelete(s_q);
        s_q = NULL;
    }
    s_inited = false;
}

esp_err_t iotmer_ble_wifi_prov_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < 300 && !ble_hs_synced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ble_hs_synced()) {
        return ESP_ERR_TIMEOUT;
    }

    char nm[32];
    if (build_gap_name(nm, sizeof(nm)) != ESP_OK) {
        strncpy(nm, "IOTMER-DEV", sizeof(nm) - 1);
    }

    s_running = true;

    ble_hs_lock();
    int rc = ble_svc_gap_device_name_set(nm);
    if (rc != 0) {
        ble_hs_unlock();
        s_running = false;
        return ESP_FAIL;
    }
    rc = start_adv_locked();
    if (rc != 0) {
        ble_hs_unlock();
        s_running = false;
        return ESP_FAIL;
    }
    ble_hs_unlock();

    return ESP_OK;
}

esp_err_t iotmer_ble_wifi_prov_stop(void)
{
    s_running = false;
    stop_idle_timer();
    stop_sess_timer();

    if (!s_inited || !ble_hs_synced()) {
        return ESP_OK;
    }

    ble_hs_lock();
    (void)ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_hs_unlock();

    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    reset_rx();
    emit_state(IOTMER_BLE_WIFI_PROV_STATE_IDLE);
    return ESP_OK;
}

#endif /* CONFIG_IOTMER_BLE_WIFI_PROV */
