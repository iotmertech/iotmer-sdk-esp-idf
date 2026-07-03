#include "iotmer_ble.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED && CONFIG_IOTMER_BLE

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#ifndef CONFIG_IOTMER_BLE_MAX_RX_LEN
#define CONFIG_IOTMER_BLE_MAX_RX_LEN 512
#endif

int iotmer_ble_gatt_init(void (*on_rx_json)(void *user_ctx, const uint8_t *data, size_t len), void *user_ctx);
esp_err_t iotmer_ble_gatt_set_tx_value(const uint8_t *data, uint16_t len);
uint16_t iotmer_ble_gatt_get_tx_handle(void);

/* Some NimBLE APIs are used without public prototypes in ESP-IDF headers. */
void ble_hs_lock(void);
void ble_hs_unlock(void);
void ble_store_config_init(void);

static const char *TAG = "iotmer_ble";

static bool s_inited;
static bool s_started;
static bool s_synced;
static bool s_connected;
static bool s_suspended;
static bool s_resume_start;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle;

static iotmer_ble_cfg_t s_cfg;
static char s_gap_name[32];

/* Optional queued RX dispatch (cfg.rx_queue_len > 0). */
typedef struct {
    uint16_t len;
    uint8_t  data[CONFIG_IOTMER_BLE_MAX_RX_LEN];
} iotmer_ble_rx_msg_t;

static QueueHandle_t s_rx_queue;
static TaskHandle_t  s_rx_task;

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void build_gap_name(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);

    const char *prefix = CONFIG_IOTMER_BLE_GAP_NAME_PREFIX;
    if (prefix == NULL) {
        prefix = "MER-";
    }

    const uint16_t suffix = ((uint16_t)mac[4] << 8) | mac[5];
    (void)snprintf(out, out_len, "%s%04X", prefix, suffix);
}

static void ble_start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected; handle=%d", (int)s_conn_handle);
            if (s_cfg.on_connect != NULL) {
                s_cfg.on_connect(s_cfg.user_ctx);
            }
        } else {
            s_connected = false;
            s_conn_handle = 0;
            if (s_started) {
                ble_start_advertising();
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected; reason=%d", (int)event->disconnect.reason);
        s_connected = false;
        s_conn_handle = 0;
        if (s_cfg.on_disconnect != NULL) {
            s_cfg.on_disconnect(s_cfg.user_ctx, (int)event->disconnect.reason);
        }
#if CONFIG_IOTMER_BLE_RESTART_ADV_ON_DISCONNECT
        if (s_started) {
            ble_start_advertising();
        }
#endif
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_started) {
            ble_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe; attr=%d notify=%d", (int)event->subscribe.attr_handle,
                 (int)event->subscribe.cur_notify);
        if (s_cfg.on_subscribe != NULL &&
            event->subscribe.attr_handle == iotmer_ble_gatt_get_tx_handle()) {
            s_cfg.on_subscribe(s_cfg.user_ctx, event->subscribe.cur_notify != 0);
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update; conn=%d mtu=%d", (int)event->mtu.conn_handle,
                 (int)event->mtu.value);
        if (s_cfg.on_mtu != NULL) {
            s_cfg.on_mtu(s_cfg.user_ctx, (uint16_t)event->mtu.value);
        }
        break;

    default:
        break;
    }

    return 0;
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Resetting state; reason=%d", reason);
    s_synced = false;
    s_connected = false;
    s_conn_handle = 0;
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE sync");
    const int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
        return;
    }

    s_synced = true;

    if (s_started) {
        ble_start_advertising();
    }
}

static void ble_start_advertising(void)
{
    if (!s_synced) {
        return;
    }

    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }

    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Advertise the IOTMER BLE JSON service UUID in the advertising PDU.
    static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
        0xee, 0xd6, 0x14, 0x1d, 0x01, 0x10, 0x00, 0x40,
        0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);
    adv.uuids128 = &svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed rc=%d", rc);
        return;
    }

    // Put GAP device name in scan response (advertising payload size constraints).
    struct ble_hs_adv_fields sr = {0};
    const char *name = ble_svc_gap_device_name();
    sr.name = (const uint8_t *)name;
    sr.name_len = strlen(name);
    sr.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&sr);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp set fields failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "Advertising started");
    } else if (rc == BLE_HS_EALREADY) {
        ESP_LOGD(TAG, "Advertising already active or start in progress");
    } else {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
    }
}

static void ble_rx_worker(void *arg)
{
    (void)arg;
    for (;;) {
        iotmer_ble_rx_msg_t msg;
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_cfg.on_rx_json != NULL) {
            s_cfg.on_rx_json(s_cfg.user_ctx, msg.data, msg.len);
        }
    }
}

static void on_rx_json(void *user_ctx, const uint8_t *data, size_t len)
{
    (void)user_ctx;

    /* Queued dispatch: hand off to the worker task so the host context stays responsive. */
    if (s_rx_queue != NULL) {
        iotmer_ble_rx_msg_t msg;
        const size_t n = len > sizeof(msg.data) ? sizeof(msg.data) : len;
        msg.len = (uint16_t)n;
        memcpy(msg.data, data, n);
        if (xQueueSend(s_rx_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "BLE RX queue full — message dropped");
        }
        return;
    }

    if (s_cfg.on_rx_json != NULL) {
        s_cfg.on_rx_json(s_cfg.user_ctx, data, len);
    }
}

static esp_err_t rx_worker_start(void)
{
    if (s_cfg.rx_queue_len == 0 || s_cfg.on_rx_json == NULL) {
        return ESP_OK; /* direct (host-context) dispatch */
    }
    if (s_rx_queue != NULL) {
        return ESP_OK; /* already running */
    }

    s_rx_queue = xQueueCreate(s_cfg.rx_queue_len, sizeof(iotmer_ble_rx_msg_t));
    if (s_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t stack = s_cfg.rx_task_stack != 0 ? s_cfg.rx_task_stack : 4096;
    if (xTaskCreate(ble_rx_worker, "iotmer_ble_rx", stack, NULL, 5, &s_rx_task) != pdPASS) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void rx_worker_stop(void)
{
    if (s_rx_task != NULL) {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }
    if (s_rx_queue != NULL) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
}

esp_err_t iotmer_ble_init(const iotmer_ble_cfg_t *cfg)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_cfg = cfg != NULL ? *cfg : IOTMER_BLE_CFG_DEFAULT();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = CONFIG_IOTMER_BLE_SMP_IO_CAP;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();

    if (s_cfg.device_name != NULL && s_cfg.device_name[0] != '\0') {
        strncpy(s_gap_name, s_cfg.device_name, sizeof(s_gap_name) - 1);
        s_gap_name[sizeof(s_gap_name) - 1] = '\0';
    } else {
        build_gap_name(s_gap_name, sizeof(s_gap_name));
    }
    err = ble_svc_gap_device_name_set(s_gap_name);
    if (err != ESP_OK) {
        return err;
    }

    const int rc = iotmer_ble_gatt_init(on_rx_json, s_cfg.user_ctx);
    if (rc != 0) {
        return ESP_FAIL;
    }

    err = rx_worker_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX worker start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = false;
    s_synced = false;
    s_connected = false;
    s_conn_handle = 0;

    nimble_port_freertos_init(ble_host_task);

    s_inited = true;
    ESP_LOGI(TAG, "init done (name=%s)", s_gap_name);
    return ESP_OK;
}

esp_err_t iotmer_ble_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    s_started = true;
    if (s_synced) {
        ble_start_advertising();
    }
    return ESP_OK;
}

esp_err_t iotmer_ble_stop(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    s_started = false;

    ble_hs_lock();
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }

    if (s_connected) {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_hs_unlock();

    s_connected = false;
    s_conn_handle = 0;
    return ESP_OK;
}

void iotmer_ble_deinit(void)
{
    if (!s_inited) {
        return;
    }

    (void)iotmer_ble_stop();

    const int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "nimble stop failed rc=%d", rc);
    }

    (void)nimble_port_deinit();

    /* Host task is stopped now; safe to tear down the RX worker. */
    rx_worker_stop();

    s_inited = false;
}

esp_err_t iotmer_ble_suspend(void)
{
    if (s_suspended) {
        return ESP_OK;
    }
    if (!s_inited) {
        s_suspended = true;
        s_resume_start = false;
        return ESP_OK;
    }

    s_resume_start = s_started;
    (void)iotmer_ble_stop();
    iotmer_ble_deinit();
    s_suspended = true;
    ESP_LOGI(TAG, "BLE radio suspended (controller RAM released)");
    return ESP_OK;
}

esp_err_t iotmer_ble_resume(void)
{
    if (!s_suspended) {
        return ESP_OK;
    }

    s_suspended = false;
    esp_err_t err = iotmer_ble_init(&s_cfg);
    if (err != ESP_OK) {
        s_suspended = true;
        ESP_LOGE(TAG, "BLE resume init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_resume_start) {
        err = iotmer_ble_start();
    }
    ESP_LOGI(TAG, "BLE radio resumed");
    return err;
}

bool iotmer_ble_is_suspended(void)
{
    return s_suspended;
}

bool iotmer_ble_is_connected(void)
{
    return s_connected;
}

uint16_t iotmer_ble_get_att_mtu(void)
{
    if (!s_inited) {
        return 0;
    }

    uint16_t mtu = 0;
    ble_hs_lock();
    if (s_connected) {
        mtu = ble_att_mtu(s_conn_handle);
    }
    ble_hs_unlock();
    return mtu;
}

esp_err_t iotmer_ble_send_json(const uint8_t *data, size_t len)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = iotmer_ble_gatt_set_tx_value(data, (uint16_t)len);
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t tx_handle = iotmer_ble_gatt_get_tx_handle();
    if (tx_handle == 0) {
        return ESP_FAIL;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ble_hs_lock();
    const bool connected = s_connected;
    const uint16_t conn_handle = s_conn_handle;
    const int rc = connected ? ble_gatts_notify_custom(conn_handle, tx_handle, om) : BLE_HS_ENOTCONN;
    ble_hs_unlock();
    if (rc != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t iotmer_ble_send_json_str(const char *json_str)
{
    if (json_str == NULL || json_str[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    return iotmer_ble_send_json((const uint8_t *)json_str, strlen(json_str));
}

#else

esp_err_t iotmer_ble_init(const iotmer_ble_cfg_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t iotmer_ble_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t iotmer_ble_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void iotmer_ble_deinit(void) {}

esp_err_t iotmer_ble_suspend(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t iotmer_ble_resume(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool iotmer_ble_is_suspended(void)
{
    return false;
}

esp_err_t iotmer_ble_send_json(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t iotmer_ble_send_json_str(const char *json_str)
{
    (void)json_str;
    return ESP_ERR_NOT_SUPPORTED;
}

bool iotmer_ble_is_connected(void)
{
    return false;
}

uint16_t iotmer_ble_get_att_mtu(void)
{
    return 0;
}

#endif

