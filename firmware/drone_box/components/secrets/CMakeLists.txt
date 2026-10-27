idf_component_register(SRCS "secrets.c"
                    INCLUDE_DIRS "include"
                    REQUIRES auth board_config nvs_flash mbedtls esp_hw_support esp_timer)
