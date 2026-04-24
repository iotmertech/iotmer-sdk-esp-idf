idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES nvs_flash esp_wifi esp_netif esp_event
                       iotmer iotmer_ble_wifi_prov)
