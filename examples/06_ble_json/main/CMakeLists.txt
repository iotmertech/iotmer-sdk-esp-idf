idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES iotmer iotmer_ble json nvs_flash esp_netif
)

