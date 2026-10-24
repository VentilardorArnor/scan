#pragma once

#include <stdbool.h>

#include "fw_err.h"

/*
 * Linha de provisionamento gerada pela plataforma para cada placa
 * (docs/MQTT.md §Provisionamento):
 *
 *   agro1://<cn>:<token>@<host>
 *
 * O token vale uma vez só e NUNCA vai para o log.
 */

#define PROV_LINE_SCHEME    "agro1://"
#define PROV_LINE_CN_MIN    (3U)
#define PROV_LINE_CN_MAX    (64U)
#define PROV_LINE_TOKEN_MIN (8U)
#define PROV_LINE_TOKEN_MAX (64U)
#define PROV_LINE_HOST_MAX  (253U)

typedef struct {
    char cn[PROV_LINE_CN_MAX + 1U];
    char token[PROV_LINE_TOKEN_MAX + 1U];
    char host[PROV_LINE_HOST_MAX + 1U];
} prov_line_t;

/**
 * @brief Separa CN, token e host.
 *
 * Valida o CN como o servidor (^[A-Za-z0-9._-]{3,64}$), o tamanho do token
 * (8..64) e o host (letras, dígitos, '.', '-' e uma porta opcional ":nnnn").
 * O token é a parte entre o primeiro ':' e o ÚLTIMO '@', como no servidor.
 *
 * @return FW_OK, FW_ERR_INVALID_PARAM ou FW_ERR_INVALID_DATA
 */
fw_err_t prov_line_parse(const char *line, prov_line_t *out);
