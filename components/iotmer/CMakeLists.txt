idf_component_register(
    SRCS
        "iotmer_client.c"
        "iotmer_telemetry.c"
        "iotmer_presence.c"
        "iotmer_topics.c"
        "iotmer_config.c"
        "iotmer_provision.c"
        "iotmer_device_auth.c"
        "iotmer_nvs.c"
        "iotmer_wifi.c"
        "iotmer_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_https_ota mqtt nvs_flash esp_wifi esp_event esp_netif esp-tls cjson app_update freertos mbedtls zlib
)

