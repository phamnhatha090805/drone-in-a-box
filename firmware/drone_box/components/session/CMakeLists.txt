idf_component_register(SRCS "session.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board_config controller actuator stepper_motor esp_timer)
