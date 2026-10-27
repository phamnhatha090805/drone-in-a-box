idf_component_register(SRCS "pair_button.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board_config secrets esp_driver_gpio esp_timer)
