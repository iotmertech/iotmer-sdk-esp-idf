/** بسم الله الرحمن الرحيم
 * Bismillahirrahmanirrahim
 * Muvaffakiyetim ise ancak Allah('ın yardımı) iledir. (Ben) yalnız O'na tevekkül ettim ve ancak O'na yönelirim (Hud/88)
 */

#include "ble_gatt_db.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "ble_json_handler.h"

#include "esp_err.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
#endif

#define BLE_JSON_MAX_LEN 512

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static uint16_t gatt_chr_val_handle;
static uint8_t g_last_json[BLE_JSON_MAX_LEN];
static uint16_t g_last_json_len;
static ble_data_handler_t g_ble_data_handler;

static int gatt_json_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, g_last_json, g_last_json_len) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int pkt_len = OS_MBUF_PKTLEN(ctxt->om);
    if (pkt_len <= 0 || pkt_len > BLE_JSON_MAX_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char *json = (char *)malloc((size_t)pkt_len + 1u);
    if (json == NULL) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    int rc = os_mbuf_copydata(ctxt->om, 0, pkt_len, json);
    if (rc == 0) {
        const uint8_t *response_data;
        uint16_t response_len;
        int should_send_response;

        json[pkt_len] = '\0';
        response_data = (const uint8_t *)json;
        response_len = (uint16_t)pkt_len;
        should_send_response = 1;

        if (g_ble_data_handler != NULL) {
            char *handled_json = g_ble_data_handler(json);

            if (handled_json == NULL) {
                should_send_response = 0;
            } else {
                response_data = (const uint8_t *)handled_json;
                response_len = (uint16_t)strnlen(handled_json, BLE_JSON_MAX_LEN);
            }
        } else {
            ble_json_handle_write((const uint8_t *)json, (uint16_t)pkt_len);
        }

        if (should_send_response) {
            memcpy(g_last_json, response_data, response_len);
            g_last_json_len = response_len;

            struct os_mbuf *om = ble_hs_mbuf_from_flat(response_data, response_len);
            if (om != NULL) {
                int notify_rc = ble_gatts_notify_custom(conn_handle, gatt_chr_val_handle, om);
                if (notify_rc != 0) {
                    ble_gatts_chr_updated(gatt_chr_val_handle);
                }
            }
        }
    }

    free(json);

    return rc == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static const ble_uuid128_t gatt_svc_uuid = BLE_UUID128_INIT(
    0x9b, 0x7b, 0x77, 0x2b, 0x5b, 0x1b, 0x47, 0x72,
    0x8a, 0x1d, 0x1b, 0x67, 0x63, 0x2d, 0x66, 0x8a);

static const ble_uuid128_t gatt_chr_uuid = BLE_UUID128_INIT(
    0x5f, 0x8b, 0x29, 0x0a, 0x7a, 0x83, 0x4e, 0x7e,
    0xb9, 0x4d, 0x41, 0x33, 0x32, 0x1c, 0x77, 0x74);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &gatt_chr_uuid.u,
                .access_cb = gatt_json_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_NOTIFY,
                // .flags = BLE_GATT_CHR_F_READ |
                //          BLE_GATT_CHR_F_WRITE |
                //          BLE_GATT_CHR_F_WRITE_NO_RSP |
                //          BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &gatt_chr_val_handle,
            },
            {0},
        },
    },
    {0},
};

int ble_gatt_db_init(ble_data_handler_t data_handler)
{
    g_ble_data_handler = data_handler;

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    return ble_gatts_add_svcs(gatt_svr_svcs);
}

esp_err_t ble_gatt_db_send_data(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > BLE_JSON_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(g_last_json, data, len);
    g_last_json_len = len;

    if (gatt_chr_val_handle != 0) {
        ble_gatts_chr_updated(gatt_chr_val_handle);
    }

    return ESP_OK;
}
#else
int ble_gatt_db_init(ble_data_handler_t data_handler)
{
    (void)data_handler;
    return -1;
}

esp_err_t ble_gatt_db_send_data(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
