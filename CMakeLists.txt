# Firmware pivo-fw: ESP32-S3, relés + GPS, contrato MQTT v2 da pivo-saas.
cmake_minimum_required(VERSION 3.22)

# Valores usados quando o sdkconfig é criado (primeiro build ou depois de
# apagá-lo). sdkconfig.local (opcional, fora do git) ajusta esta máquina.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pivo_fw)
