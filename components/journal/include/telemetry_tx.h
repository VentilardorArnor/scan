#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "contract_json.h"
#include "journal.h"

/*
 * Envio confiável da telemetria (ADR-002, §5 do contrato).
 *
 * O PUBACK do broker não libera nada: um envelope só sai do journal com
 * TelemetryAck `stored` ou `duplicate` para o mesmo (boot_id, seq).
 */

#define TELEMETRY_TX_WINDOW            (CONTRACT_TELEMETRY_WINDOW)
#define TELEMETRY_TX_ACK_TIMEOUT_MS    (CONTRACT_TELEMETRY_ACK_TIMEOUT_MS)
#define TELEMETRY_TX_BACKOFF_START_MS  (30000U)
#define TELEMETRY_TX_BACKOFF_MAX_MS    (CONTRACT_BACKOFF_MAX_MS)
#define TELEMETRY_TX_SKEW_HOLD_MS      (30000U) /* espera o relógio ressincronizar */

typedef enum {
    TELEMETRY_TX_EV_REJECTED_INVALID = 0, /* lote apagado: publicar telemetry_rejected */
    TELEMETRY_TX_EV_CLOCK_SKEW,           /* ressincronizar o relógio */
    TELEMETRY_TX_EV_UNCLAIMED,            /* dispositivo ainda sem claim */
    TELEMETRY_TX_EV_REVOKED,              /* telemetria parada até reiniciar */
    TELEMETRY_TX_EV_ACCEPTED,             /* primeiro stored/duplicate do boot ou depois de recusa */
} telemetry_tx_event_t;

/** @brief Publica um envelope em /telemetry. Devolve false se não deu para enfileirar. */
typedef bool (*telemetry_tx_publish_fn)(void *ctx, const char *payload, size_t len);

/** @brief Avisa o aplicativo de algo que exige ação fora deste módulo. */
typedef void (*telemetry_tx_event_fn)(void *ctx, telemetry_tx_event_t event,
                                      const journal_key_t *key);

typedef struct {
    journal_key_t key;
    uint64_t sent_ms;
    bool used;
} telemetry_tx_slot_t;

/** @brief Contadores para diagnóstico (nunca zerados durante o boot). */
typedef struct {
    uint32_t sent;            /* primeira publicação de um lote */
    uint32_t resent;          /* reenvio por falta de ACK */
    uint32_t publish_refused; /* fila de saída cheia */
    uint32_t ack_stored;
    uint32_t ack_duplicate;
    uint32_t ack_invalid;
    uint32_t ack_unclaimed;
    uint32_t ack_revoked;
    uint32_t ack_clock_skew;
    uint32_t ack_unknown;     /* rejected sem motivo conhecido */
    uint32_t ack_not_found;   /* ACK de lote que não está entre os pendentes */
    uint32_t ack_other_err;   /* journal_ack recusou por outro motivo (lista cheia, E/S) */
    int32_t last_ack_err;     /* último código de erro do journal_ack */
} telemetry_tx_stats_t;

typedef struct {
    journal_t *journal;
    telemetry_tx_publish_fn publish;
    telemetry_tx_event_fn on_event;
    void *ctx;
    telemetry_tx_slot_t slots[TELEMETRY_TX_WINDOW];
    uint64_t hold_until_ms;
    uint32_t backoff_ms;
    bool revoked;
    bool accepting; /* último ACK foi stored/duplicate */
    telemetry_tx_stats_t stats;
} telemetry_tx_t;

/** @brief Prepara o transmissor sobre um journal já aberto. */
void telemetry_tx_init(telemetry_tx_t *tx, journal_t *journal, telemetry_tx_publish_fn publish,
                       telemetry_tx_event_fn on_event, void *ctx);

/**
 * @brief Envia ou reenvia os pendentes mais antigos, respeitando a janela.
 *
 * Chamar periodicamente. Sem ACK em TELEMETRY_TX_ACK_TIMEOUT_MS, o mesmo lote
 * (mesmo boot_id e seq) é reenviado.
 *
 * @param payload_buf Área de trabalho para ler o envelope do journal
 */
void telemetry_tx_poll(telemetry_tx_t *tx, uint64_t now_ms, bool connected, char *payload_buf,
                       size_t payload_cap);

/** @brief Trata um TelemetryAck recebido. */
void telemetry_tx_on_ack(telemetry_tx_t *tx, const contract_telemetry_ack_t *ack, uint64_t now_ms);

/** @brief Nova conexão ao broker: tudo que estava em voo deve ser reenviado. */
void telemetry_tx_on_connect(telemetry_tx_t *tx);
