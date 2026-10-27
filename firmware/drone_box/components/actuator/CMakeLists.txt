idf_component_register(SRCS "actuator.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board_config command esp_driver_gpio esp_driver_ledc)
