/** بسم الله الرحمن الرحيم
 * Bismillahirrahmanirrahim
 * Muvaffakiyetim ise ancak Allah('ın yardımı) iledir. (Ben) yalnız O'na tevekkül ettim ve ancak O'na yönelirim (Hud/88)
 */

#include "ble_json_handler.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "ble_json";

void ble_json_handle_write(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    char *buf = (char *)malloc((size_t)len + 1u);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Out of memory for JSON buffer");
        return;
    }

    memcpy(buf, data, len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "RX JSON: %s", buf);

    free(buf);
}
