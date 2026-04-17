#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

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

    if (esp_err == ESP_OK) {
        esp_err_t w = iotmer_wifi_reconnect();
        ESP_LOGI(TAG, "iotmer_wifi_reconnect: %s", esp_err_to_name(w));
        (void)iotmer_ble_wifi_prov_stop();
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

    ESP_LOGI(TAG, "BLE Wi-Fi provisioning active — use mobile app or nRF Connect (see docs).");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
