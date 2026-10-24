# Camada de aplicação: sequência de partida e as tarefas FreeRTOS.
idf_component_register(
    SRCS "app_main.c" "app_bus.c" "wifi_sta.c"
         "relay_task.c" "gps_task.c" "journal_task.c"
         "mqtt_task.c" "cmd_task.c" "health_task.c" "provision_task.c"
         "device_state.c" "button_task.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fw_common contract gps_nmea journal control prov_line enroll
                  relays mqtt_link timekeeping ota
                  nvs_flash esp_wifi esp_netif esp_event esp_timer esp_system
                  esp_driver_uart esp_driver_gpio esp_app_format bootloader_support
                  espressif__network_provisioning protocomm bt espressif__cjson mbedtls
                  joltwallet__littlefs)
