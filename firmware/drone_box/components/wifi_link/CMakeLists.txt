idf_component_register(SRCS "wifi_link.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board_config secrets esp_wifi esp_event esp_netif esp_timer)
