# board_config only, and only for its header: this component must stay free of
# ESP-IDF so it can be built and unit tested on a host. board_config includes
# nothing and links nothing, so it does not cost that.
idf_component_register(SRCS "command.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board_config)
