#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wi-Fi, provision (when configured), MQTT connect, command subscription, telemetry loop.
 * Runs after hw_init(). Blocks in a telemetry task — do not call twice.
 */
esp_err_t iotmer_app_start(void);

#ifdef __cplusplus
}
#endif
