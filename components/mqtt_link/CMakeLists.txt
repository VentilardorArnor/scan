idf_component_register(SRCS "mqtt_link.c" "identity.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fw_common contract
                       PRIV_REQUIRES espressif__mqtt nvs_flash esp_timer)
