idf_component_register(SRCS "enroll.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fw_common
                       PRIV_REQUIRES prov_line mqtt_link espressif__cjson esp_http_client
                                     mbedtls nvs_flash)
