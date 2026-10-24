#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "fw_err.h"
#include "identity.h"

/*
 * Conexão MQTT 3.1.1 com mTLS ao broker da plataforma.
 *
 * Só toca nos tópicos liberados pela ACL: publicar ou assinar qualquer outro
 * derruba a conexão (deny_action = disconnect). Os tópicos de controle
 * (cmd, config, state, cmd/ack) só existem com `control_topics`, ligado em
 * toda placa com relés, tenha ela acionamento ou não.
 */

#define MQTT_LINK_RX_MAX (2048U) /* maior mensagem recebida aceita */

typedef enum {
    MQTT_LINK_TOPIC_TELEMETRY = 0,
    MQTT_LINK_TOPIC_TELEMETRY_ACK,
    MQTT_LINK_TOPIC_STATUS,
    MQTT_LINK_TOPIC_EVENT,
    MQTT_LINK_TOPIC_STATE,   /* controle */
    MQTT_LINK_TOPIC_CMD,     /* controle */
    MQTT_LINK_TOPIC_CMD_ACK, /* controle */
    MQTT_LINK_TOPIC_CONFIG,  /* controle */
    MQTT_LINK_TOPIC_COUNT,
} mqtt_link_topic_t;

/** @brief Mensagem recebida. Roda na tarefa do cliente MQTT: só enfileire. */
typedef void (*mqtt_link_rx_cb_t)(mqtt_link_topic_t topic, const char *data, size_t len);

/** @brief Mudança de estado da conexão. Roda na tarefa do cliente MQTT. */
typedef void (*mqtt_link_conn_cb_t)(bool connected);

typedef struct {
    const char *uri; /* mqtts://host:8883 */
    const identity_t *identity;
    bool control_topics;
    mqtt_link_rx_cb_t on_rx;
    mqtt_link_conn_cb_t on_conn;
} mqtt_link_config_t;

/** @brief Cria o cliente (sem conectar). Os ponteiros precisam continuar válidos. */
fw_err_t mqtt_link_init(const mqtt_link_config_t *config);

/** @brief Primeira conexão. */
fw_err_t mqtt_link_start(void);

/** @brief Nova tentativa depois de uma queda (o backoff é de quem chama). */
fw_err_t mqtt_link_reconnect(void);

/**
 * @brief Confere os SUBACKs: reassina o que não foi confirmado em 10 s ou foi
 *        recusado pelo broker há mais de 60 s. Chamar periodicamente.
 */
void mqtt_link_poll(void);

/** @brief true entre CONNECTED e DISCONNECTED. Seguro de qualquer tarefa. */
bool mqtt_link_is_connected(void);

/**
 * @brief Enfileira uma publicação QoS 1.
 *
 * @return FW_OK; FW_ERR_STATE se desconectado ou se o tópico é de controle e
 *         o controle está desligado; FW_ERR_FULL se a fila do cliente encheu
 */
fw_err_t mqtt_link_publish(mqtt_link_topic_t topic, const char *payload, size_t len, bool retain);
