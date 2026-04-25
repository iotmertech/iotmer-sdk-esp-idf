idf_component_register(
    SRCS
        "iotmer_ble.c"
        "iotmer_ble_gatt.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        bt
        nvs_flash
)

