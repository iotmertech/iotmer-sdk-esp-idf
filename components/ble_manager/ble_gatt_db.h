/** بسم الله الرحمن الرحيم
 * Bismillahirrahmanirrahim
 * Muvaffakiyetim ise ancak Allah('ın yardımı) iledir. (Ben) yalnız O'na tevekkül ettim ve ancak O'na yönelirim (Hud/88)
 */

#ifndef BLE_GATT_DB_H
#define BLE_GATT_DB_H

#include "ble_manager.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ble_gatt_db_init(ble_data_handler_t data_handler);
esp_err_t ble_gatt_db_send_data(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}   
#endif

#endif // BLE_GATT_DB_H
