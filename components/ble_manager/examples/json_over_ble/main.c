#include <stdio.h>
#include "ble_manager.h"
#include "esp_log.h"
#include "esp_err.h"

static char *ble_json_data_handler(char *value)
{
	printf("Received JSON over BLE: %s\n", value);
	// Here you can parse the JSON and perform actions based on its content.
	// For demonstration, we just return NULL to indicate no response.
	ble_manager_send_data("Test data received");
	return value;
}

void app_main(void)
{
	const char *device_name = "Iotmer Power Device";
	esp_err_t err = ble_manager_init(device_name, ble_json_data_handler);
	if (err == ESP_OK) {
		ble_manager_start();
        ESP_LOGI("main", "BLE manager started");
	} else {
		ESP_LOGE("main", "Failed to initialize BLE manager: %s", esp_err_to_name(err));
    }
}