idf_component_register(SRCS "ble_manager.c"
                            "ble_gatt_db.c"
                            "ble_json_handler.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bt nvs_flash)
