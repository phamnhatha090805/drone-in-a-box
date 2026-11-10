idf_component_register(SRCS "stepper_motor.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board_config esp_driver_gpio esp_timer)
