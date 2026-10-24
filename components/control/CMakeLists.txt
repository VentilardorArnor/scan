# Lógica portátil: sem dependência do ESP-IDF, compila também nos testes de host.
idf_component_register(SRCS "control.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fw_common contract)
