idf_component_register(SRCS "ble_link.c"
                    INCLUDE_DIRS "include"
                    REQUIRES auth board_config controller secrets session wifi_link bt esp_timer
                             nvs_flash)
