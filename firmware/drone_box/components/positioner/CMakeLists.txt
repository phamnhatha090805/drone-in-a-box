idf_component_register(SRCS "positioner.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board_config actuator esp_timer)
