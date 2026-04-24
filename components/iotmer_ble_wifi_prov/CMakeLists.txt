# Do not gate REQUIRES on CONFIG_* here: component requirements are resolved before sdkconfig
# values are stable for optional Kconfig symbols, which breaks NimBLE include paths (bt).
# Kconfig still controls the implementation via CONFIG_IOTMER_BLE_WIFI_PROV in the .c file.
idf_component_register(
    SRCS "iotmer_ble_wifi_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_timer iotmer nvs_flash freertos esp_common cjson
    PRIV_REQUIRES bt
)
