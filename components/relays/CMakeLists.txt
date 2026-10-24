idf_component_register(SRCS "relay_hw.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fw_common
                       PRIV_REQUIRES esp_driver_gpio esp_timer esp_system)
