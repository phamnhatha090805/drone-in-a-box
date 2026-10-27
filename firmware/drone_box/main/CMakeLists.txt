idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES auth ble_link command controller actuator pair_button positioner secrets serial_io session webserver wifi_link)
