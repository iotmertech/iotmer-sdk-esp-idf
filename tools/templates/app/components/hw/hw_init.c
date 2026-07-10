#include "esp_check.h"
#include "esp_log.h"

#include "driver/gpio.h"

#include "hw_config.h"

static const char *TAG = "hw";

esp_err_t hw_init(void)
{
    /*
     * Example: status LED on CONFIG_HW_STATUS_LED_GPIO (menuconfig → HW).
     * Replace with your PCB init — buses, PHY reset, RS485 DE pin, etc.
     */
#if CONFIG_HW_STATUS_LED_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_HW_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "status LED gpio_config");
    gpio_set_level(CONFIG_HW_STATUS_LED_GPIO, 0);
    ESP_LOGI(TAG, "status LED on GPIO %d", CONFIG_HW_STATUS_LED_GPIO);
#else
    ESP_LOGI(TAG, "hw_init: no status LED configured (set HW → Status LED GPIO)");
#endif

    return ESP_OK;
}

esp_err_t hw_read_telemetry(char *json_fields, size_t json_fields_len)
{
    if (json_fields == NULL || json_fields_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    json_fields[0] = '\0';

    /* Example: append sensor readings to the telemetry JSON payload. */
    /* snprintf(json_fields, json_fields_len, "\"temp\":%.1f", read_temp()); */

    return ESP_OK;
}
