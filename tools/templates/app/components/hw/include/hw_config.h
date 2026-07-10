#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize board-specific hardware (GPIO, I2C, RS485, Ethernet PHY, etc.).
 * Called before any IOTMER / TLS work. Keep this fast and allocation-light.
 */
esp_err_t hw_init(void);

/**
 * Optional periodic hook from the cloud telemetry loop.
 * Read sensors and return a JSON object body (without outer braces), e.g. "\"temp\":22.5".
 * Return ESP_OK with empty string to skip extra fields.
 */
esp_err_t hw_read_telemetry(char *json_fields, size_t json_fields_len);

#ifdef __cplusplus
}
#endif
