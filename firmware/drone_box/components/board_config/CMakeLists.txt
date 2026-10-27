# Header-only: every tunable constant for this board and this deployment.
#
# No SRCS and no REQUIRES on purpose. Nothing links against board_config, so
# adding it to a component's REQUIRES costs nothing but the include path, and
# `command` and `auth` can keep their promise of building on the host.
idf_component_register(INCLUDE_DIRS "include")
