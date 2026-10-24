#pragma once

#include <stdint.h>

#include "contract.h"
#include "fw_err.h"

/*
 * Identidade do dispositivo, gravada pelo enroll no namespace `identity` da
 * partição `nvs`: CN (= clientid), chave privada, certificado, CA raiz que
 * valida o broker e o endereço do broker. Nada disso está no código nem no
 * git, e a chave privada nunca aparece em log.
 */

#define IDENTITY_NAMESPACE      "identity"
#define IDENTITY_KEY_LINE_SHA   "line_sha"  /* SHA-256 da linha usada no enroll */
#define IDENTITY_KEY_DEVICE_ID  "device_id"
#define IDENTITY_KEY_KEY        "key"       /* PEM da chave EC P-256 */
#define IDENTITY_KEY_CERT       "cert"      /* PEM do certificado de cliente */
#define IDENTITY_KEY_CA         "ca"        /* PEM da raiz (último do ca_chain) */
#define IDENTITY_KEY_MQTT_HOST  "mqtt_host"
#define IDENTITY_KEY_MQTT_PORT  "mqtt_port"
#define IDENTITY_HOST_MAX       (253U)

typedef struct {
    char device_id[CONTRACT_DEVICE_ID_MAX_LEN + 1U];
    char mqtt_host[IDENTITY_HOST_MAX + 1U];
    uint16_t mqtt_port;
    const char *cert_pem; /* PEM terminado em NUL */
    const char *key_pem;
    const char *ca_pem;
} identity_t;

/**
 * @brief Lê a identidade completa (só existe depois de um enroll bem-sucedido).
 *
 * Os PEM ficam em memória alocada uma vez e nunca liberada: a conexão TLS
 * precisa deles durante toda a vida do firmware.
 *
 * @return FW_OK; FW_ERR_NOT_FOUND se ainda não houve enroll;
 *         FW_ERR_INVALID_DATA se o CN tem caractere inválido para tópico
 */
fw_err_t identity_load(identity_t *out);

/** @brief Libera os PEM de uma identidade que não vai ser usada para conectar. */
void identity_release(identity_t *identity);
