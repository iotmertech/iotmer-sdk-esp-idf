idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES iotmer iotmer_ble cjson nvs_flash esp_netif
)

