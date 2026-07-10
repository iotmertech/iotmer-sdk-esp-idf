idf_component_register(
    SRCS "hw_init.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_common
)
