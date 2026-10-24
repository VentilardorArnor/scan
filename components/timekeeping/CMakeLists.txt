idf_component_register(SRCS "timekeeping.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_netif esp_timer lwip)
