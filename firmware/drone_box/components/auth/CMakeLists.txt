# No REQUIRES: this component must stay free of ESP-IDF so it can be built and
# unit tested on a host. The HMAC primitive is injected by the caller.
idf_component_register(SRCS "auth.c"
                    INCLUDE_DIRS "include")
