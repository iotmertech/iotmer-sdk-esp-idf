#include "iotmer_ble.h"

#include <string.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "esp_err.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED && CONFIG_IOTMER_BLE

#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#ifndef CONFIG_IOTMER_BLE_MAX_RX_LEN
#define CONFIG_IOTMER_BLE_MAX_RX_LEN 512
#endif

typedef struct {
    uint16_t tx_val_handle;
    uint16_t rx_val_handle;

    uint8_t last_tx[CONFIG_IOTMER_BLE_MAX_RX_LEN];
    uint16_t last_tx_len;

    void (*on_rx_json)(void *user_ctx, const uint8_t *data, size_t len);
    void *user_ctx;
} iotmer_ble_gatt_state_t;

static iotmer_ble_gatt_state_t s_gatt;

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xee, 0xd6, 0x14, 0x1d, 0x01, 0x10, 0x00, 0x40,
    0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);

static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0xee, 0xd6, 0x14, 0x1d, 0x02, 0x10, 0x00, 0x40,
    0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);

static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0xee, 0xd6, 0x14, 0x1d, 0x03, 0x10, 0x00, 0x40,
    0x80, 0x24, 0xb5, 0xa3, 0xc0, 0xff, 0xee, 0x01);

static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return os_mbuf_append(ctxt->om, s_gatt.last_tx, s_gatt.last_tx_len) == 0 ? 0
                                                                             : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const int pkt_len = OS_MBUF_PKTLEN(ctxt->om);
    if (pkt_len <= 0 || pkt_len > CONFIG_IOTMER_BLE_MAX_RX_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t buf[CONFIG_IOTMER_BLE_MAX_RX_LEN];
    const int rc = os_mbuf_copydata(ctxt->om, 0, pkt_len, buf);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (s_gatt.on_rx_json != NULL) {
        s_gatt.on_rx_json(s_gatt.user_ctx, buf, (size_t)pkt_len);
    }

    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_rx_uuid.u,
                    .access_cb = rx_access_cb,
#if CONFIG_IOTMER_BLE_REQUIRE_ENC
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
#else
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
#endif
                    .val_handle = &s_gatt.rx_val_handle,
                },
                {
                    .uuid = &s_tx_uuid.u,
                    .access_cb = tx_access_cb,
#if CONFIG_IOTMER_BLE_REQUIRE_ENC
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
#else
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
#endif
                    .val_handle = &s_gatt.tx_val_handle,
                },
                {0},
            },
    },
    {0},
};

int iotmer_ble_gatt_init(void (*on_rx_json)(void *user_ctx, const uint8_t *data, size_t len), void *user_ctx)
{
    memset(&s_gatt, 0, sizeof(s_gatt));
    s_gatt.on_rx_json = on_rx_json;
    s_gatt.user_ctx = user_ctx;

    const int rc1 = ble_gatts_count_cfg(s_svcs);
    if (rc1 != 0) {
        return rc1;
    }

    return ble_gatts_add_svcs(s_svcs);
}

esp_err_t iotmer_ble_gatt_set_tx_value(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > CONFIG_IOTMER_BLE_MAX_RX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_gatt.last_tx, data, len);
    s_gatt.last_tx_len = len;

    if (s_gatt.tx_val_handle != 0) {
        ble_gatts_chr_updated(s_gatt.tx_val_handle);
    }

    return ESP_OK;
}

uint16_t iotmer_ble_gatt_get_tx_handle(void)
{
    return s_gatt.tx_val_handle;
}

#else

int iotmer_ble_gatt_init(void (*on_rx_json)(void *user_ctx, const uint8_t *data, size_t len), void *user_ctx)
{
    (void)on_rx_json;
    (void)user_ctx;
    return -1;
}

esp_err_t iotmer_ble_gatt_set_tx_value(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

uint16_t iotmer_ble_gatt_get_tx_handle(void)
{
    return 0;
}

#endif

