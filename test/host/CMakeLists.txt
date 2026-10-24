# Testes de host: rodam no PC, sem placa e sem ESP-IDF.
#
#   cmake -S test/host -B build-host && cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Unity e cJSON são baixados na primeira configuração, com versão fixada.
cmake_minimum_required(VERSION 3.22)
project(pivo_fw_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.1
    GIT_SHALLOW    TRUE)
FetchContent_Declare(cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE)
# Só os fontes: os CMakeLists dos dois projetos trazem alvos que não usamos.
FetchContent_Populate(unity)
FetchContent_Populate(cjson)

set(COMPONENTS ${CMAKE_CURRENT_SOURCE_DIR}/../../components)

add_library(third_party STATIC
    ${unity_SOURCE_DIR}/src/unity.c
    ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(third_party PUBLIC
    ${unity_SOURCE_DIR}/src
    ${cjson_SOURCE_DIR})
target_compile_definitions(third_party PUBLIC UNITY_INCLUDE_DOUBLE)

add_library(fw_portable STATIC
    ${COMPONENTS}/fw_common/fw_crc32.c
    ${COMPONENTS}/fw_common/fw_time.c
    ${COMPONENTS}/contract/contract_json.c
    ${COMPONENTS}/gps_nmea/nmea.c
    ${COMPONENTS}/gps_nmea/gps_track.c
    ${COMPONENTS}/journal/journal.c
    ${COMPONENTS}/journal/telemetry_tx.c
    ${COMPONENTS}/control/control.c
    ${COMPONENTS}/prov_line/prov_line.c)
target_include_directories(fw_portable PUBLIC
    ${COMPONENTS}/fw_common/include
    ${COMPONENTS}/contract/include
    ${COMPONENTS}/gps_nmea/include
    ${COMPONENTS}/journal/include
    ${COMPONENTS}/control/include
    ${COMPONENTS}/prov_line/include)
target_link_libraries(fw_portable PUBLIC third_party m)
target_compile_options(fw_portable PRIVATE -Wall -Wextra -Werror)

add_executable(host_tests
    test_main.c
    test_nmea.c
    test_contract.c
    test_journal.c
    test_telemetry_tx.c
    test_control.c
    test_prov_line.c
    test_lossy_link.c)
target_link_libraries(host_tests PRIVATE fw_portable)
target_compile_options(host_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
