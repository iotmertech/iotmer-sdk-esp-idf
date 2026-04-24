/** بسم الله الرحمن الرحيم
 * Bismillahirrahmanirrahim
 * Muvaffakiyetim ise ancak Allah('ın yardımı) iledir. (Ben) yalnız O'na tevekkül ettim ve ancak O'na yönelirim (Hud/88)
 */

#include "ble_manager.h"
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "ble_gatt_db.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#endif

void ble_store_config_init(void);

static const char *TAG = "ble_mgr";

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static const char *DEFAULT_BLE_DEVICE_NAME = "Antcom Smart Device";
static uint8_t s_own_addr_type;
static bool s_ble_started;
static bool s_ble_synced;
static bool s_ble_connected;

static void ble_start_advertising(void);

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble_connected = true;
        } else if (s_ble_started) {
            ble_start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_ble_connected = false;
        if (s_ble_started) {
            ble_start_advertising();
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_ble_started) {
            ble_start_advertising();
        }
        break;
    default:
        break;
    }

    return 0;
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "Resetting state; reason=%d", reason);
    s_ble_synced = false;
}

static void ble_start_advertising(void)
{
    ESP_LOGI(TAG, "Starting advertising");
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising started");
    }
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE sync");
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
        return;
    }

    s_ble_synced = true;

    if (s_ble_started) {
        ble_start_advertising();
    }
}

esp_err_t ble_manager_init(const char *device_name, ble_data_handler_t data_handler)
{
    const char *resolved_device_name = device_name;

    if (resolved_device_name == NULL || resolved_device_name[0] == '\0') {
        resolved_device_name = DEFAULT_BLE_DEVICE_NAME;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_store_config_init();

    err = ble_svc_gap_device_name_set(resolved_device_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %s", esp_err_to_name(err));
        return err;
    }

    int rc = ble_gatt_db_init(data_handler);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatt_db_init failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_ble_started = false;
    s_ble_synced = false;
    s_ble_connected = false;

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE init done");

    return ESP_OK;
}

esp_err_t ble_manager_start(void)
{
    s_ble_started = true;

    if (s_ble_synced) {
        ble_start_advertising();
    }

    ESP_LOGI(TAG, "BLE start");

    return ESP_OK;
}

esp_err_t ble_manager_stop(void)
{
    s_ble_started = false;
    s_ble_connected = false;

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }

    ESP_LOGI(TAG, "BLE stop");

    return ESP_OK;
}

esp_err_t ble_manager_send_data(const char *data)
{
    if (data == NULL || data[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ble_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    

    size_t len = strlen(data);
    if (len > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ble_gatt_db_send_data((const uint8_t *)data, (uint16_t)len);
}

esp_err_t ble_manager_deinit(void)
{
    ble_manager_stop();

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "nimble stop failed rc=%d", rc);
    }

    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "BLE deinit done");

    return ESP_OK;
}
#else
esp_err_t ble_manager_init(const char *device_name, ble_data_handler_t data_handler)
{
    (void)device_name;
    (void)data_handler;
    ESP_LOGE(TAG, "BLE/NimBLE disabled. Enable CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED.");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_manager_start(void)
{
    ESP_LOGE(TAG, "BLE/NimBLE disabled. Enable CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED.");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_manager_stop(void)
{
    ESP_LOGE(TAG, "BLE/NimBLE disabled. Enable CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED.");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_manager_send_data(const char *data)
{
    (void)data;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_manager_deinit(void)
{
    return ESP_OK;
}
#endif
