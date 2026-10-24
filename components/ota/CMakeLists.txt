idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fw_common
                       PRIV_REQUIRES app_update)
