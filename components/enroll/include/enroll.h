#pragma once

#include <stdint.h>

#include "fw_err.h"

/*
 * Primeira conexão da placa (docs/MQTT.md §Provisionamento): troca a linha
 * `agro1://<cn>:<token>@<host>` por um certificado de cliente.
 *
 * 1. gera a chave EC P-256 na placa e a grava no NVS ANTES de chamar o
 *    servidor (se cair no meio, o reenvio usa a mesma chave e recebe o mesmo
 *    certificado, `reissued: true`);
 * 2. monta a CSR com CN = o CN da linha;
 * 3. POST https://<host>/v1/devices/enroll, validando o servidor pelo
 *    esp_crt_bundle (certificado público);
 * 4. grava certificado, raiz do `ca_chain` e host/porta do MQTT.
 *
 * O token e a chave privada nunca vão para o log.
 */

typedef enum {
    ENROLL_DONE = 0,       /* identidade gravada (agora ou antes) */
    ENROLL_RETRY,          /* rede, 429, 503 ou 5xx: tentar de novo mais tarde */
    ENROLL_NEED_NEW_LINE,  /* 404 ou 410: parar; precisa de linha nova */
    ENROLL_FIRMWARE_ERROR, /* 422, resposta ilegível, linha malformada: parar */
} enroll_result_t;

/**
 * @brief Garante a identidade para esta linha, chamando o servidor se preciso.
 *
 * Bloqueia durante a requisição HTTPS (até ~15 s). Precisa de rede e de hora
 * válida (o TLS confere a validade do certificado do servidor).
 *
 * Se a linha gravada no firmware for outra (placa regravada com linha nova),
 * a identidade anterior é apagada e o enroll começa do zero.
 *
 * @param line        Linha de provisionamento (AGRO_PROVISIONING)
 * @param http_status Recebe o status HTTP da última tentativa (0 se não houve)
 */
enroll_result_t enroll_ensure(const char *line, int32_t *http_status);
