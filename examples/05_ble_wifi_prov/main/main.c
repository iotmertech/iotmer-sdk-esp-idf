#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "iotmer_client.h"
#include "iotmer_ble_wifi_prov.h"
#include "iotmer_wifi.h"

static const char *TAG = "main";

static void on_state(void *ctx, iotmer_ble_wifi_prov_state_t st)
{
    (void)ctx;
    ESP_LOGI(TAG, "BLE prov state: %d", (int)st);
}

static void on_result(void *ctx, esp_err_t esp_err, iotmer_ble_wifi_prov_err_t detail)
{
    (void)ctx;
    ESP_LOGI(TAG, "BLE prov result: esp_err=%s detail=%u", esp_err_to_name(esp_err), (unsigned)detail);

    if (esp_err != ESP_OK) {
        return;
    }

    esp_err_t w = iotmer_wifi_reconnect();
    ESP_LOGI(TAG, "iotmer_wifi_reconnect: %s", esp_err_to_name(w));
    (void)iotmer_ble_wifi_prov_stop();

    iotmer_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    (void)iotmer_nvs_load_creds(&creds);

    bool      https = false;
    esp_err_t pr    = iotmer_provision(&creds, &https);
    if (pr != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_provision: %s (set IOTMER_PROVISION_* in menuconfig for this demo)",
                 esp_err_to_name(pr));
        return;
    }
    pr = iotmer_nvs_save_creds(&creds);
    if (pr != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_nvs_save_creds: %s", esp_err_to_name(pr));
    }

    const char        *claim_src = NULL;
    char               claim_buf[IOTMER_BLE_WIFI_PROV_CLAIM_MAX];
    bool                claim_from_gatt = false;

    if (iotmer_ble_wifi_prov_get_claim_code(claim_buf, sizeof(claim_buf)) == ESP_OK) {
        claim_src       = claim_buf;
        claim_from_gatt = true;
    } else if (CONFIG_IOTMER_05_DEMO_CLAIM_CODE[0] != '\0') {
        claim_src = CONFIG_IOTMER_05_DEMO_CLAIM_CODE;
    }

    if (claim_src) {
        esp_err_t br = iotmer_device_auth_bind_claim(&creds, claim_src);
        if (claim_from_gatt) {
            memset(claim_buf, 0, sizeof(claim_buf));
        }
        if (br == ESP_OK) {
            ESP_LOGI(TAG, "bind-claim ok");
            if (claim_from_gatt) {
                iotmer_ble_wifi_prov_clear_claim_code();
            }
        } else {
            ESP_LOGW(TAG, "bind-claim: %s", esp_err_to_name(br));
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    iotmer_ble_wifi_prov_cfg_t cfg = IOTMER_BLE_WIFI_PROV_CFG_DEFAULT();
    cfg.on_state  = on_state;
    cfg.on_result = on_result;

    err = iotmer_ble_wifi_prov_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_ble_wifi_prov_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = iotmer_ble_wifi_prov_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iotmer_ble_wifi_prov_start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG,
             "BLE: Wi-Fi + optional Claim (…0404…); or set IOTMER_05_DEMO_CLAIM_CODE for lab bind");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
