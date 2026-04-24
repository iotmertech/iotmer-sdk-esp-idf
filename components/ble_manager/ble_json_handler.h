/** بسم الله الرحمن الرحيم
 * Bismillahirrahmanirrahim
 * Muvaffakiyetim ise ancak Allah('ın yardımı) iledir. (Ben) yalnız O'na tevekkül ettim ve ancak O'na yönelirim (Hud/88)
 */

#ifndef BLE_JSON_HANDLER_H
#define BLE_JSON_HANDLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ble_json_handle_write(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif // BLE_JSON_HANDLER_H
