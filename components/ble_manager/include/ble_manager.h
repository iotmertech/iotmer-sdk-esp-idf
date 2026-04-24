/** بسم الله الرحمن الرحيم
 * Bismillahirrahmanirrahim
 * Muvaffakiyetim ise ancak Allah('ın yardımı) iledir. (Ben) yalnız O'na tevekkül ettim ve ancak O'na yönelirim (Hud/88)
 */

#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef char *(*ble_data_handler_t)(char *value);

/**
 * @brief Initialize the BLE manager
 * @param device_name The name to be advertised by the BLE device. If NULL, a default name will be used.
 * @param data_handler Callback invoked with JSON payloads received over BLE.
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t ble_manager_init(const char *device_name, ble_data_handler_t data_handler);

/**
 * @brief Start BLE advertising. If the BLE manager is not initialized, this function will return an error.
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t ble_manager_start(void);

/**
 * @brief Stop BLE advertising. If the BLE manager is not initialized, this function will return an error.
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t ble_manager_stop(void);

/**
 * @brief Send data directly over BLE characteristic.
 * @param data Null-terminated payload to publish over BLE.
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t ble_manager_send_data(const char *data);

/**
 * @brief Deinitialize the BLE manager. If the BLE manager is not initialized, this function will return an error.
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t ble_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_MANAGER_H

