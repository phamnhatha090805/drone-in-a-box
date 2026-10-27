idf_component_register(SRCS "webserver.c"
                    INCLUDE_DIRS "include"
                    REQUIRES auth command controller actuator secrets session wifi_link
                             esp_http_server esp_timer mbedtls
                    EMBED_FILES "index.html")
