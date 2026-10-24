# Lógica portátil: sem dependência do ESP-IDF, compila também nos testes de host.
idf_component_register(SRCS "prov_line.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fw_common)
