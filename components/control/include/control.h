#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "contract.h"
#include "contract_json.h"
#include "fw_err.h"

/*
 * Controle do painel do pivô (ADR-007, docs/MQTT.md §Comandos): validação da
 * `config`, regras de cada `cmd` e idempotência por `id`. Sem acesso a
 * hardware: decide e quem chama executa.
 */

typedef enum {
    CONTROL_KIND_START = 0,
    CONTROL_KIND_STOP,
    CONTROL_KIND_FORWARD,
    CONTROL_KIND_REVERSE,
    CONTROL_KIND_WATER_ON,
    CONTROL_KIND_WATER_OFF,
    CONTROL_KIND_CUSTOM,
} control_kind_t;

typedef struct {
    uint8_t channel; /* 0..15 */
    control_kind_t kind;
    uint16_t pulse_ms; /* 100..5000: teto do pulso deste canal */
} control_channel_t;

/* DeviceConfig (config.schema.json) */
typedef struct {
    uint32_t version;
    bool estop;
    uint16_t failsafe_s;
    uint8_t channel_count;
    control_channel_t channels[CONTRACT_CHANNEL_MAX];
} control_config_t;

/**
 * @brief Valida a config inteira e só então preenche `out`.
 *
 * Campos desconhecidos são ignorados. Em erro `out` não muda: a config
 * anterior continua valendo.
 *
 * @param detail Se não NULL, recebe um texto curto (literal) com o motivo
 */
fw_err_t control_config_parse(const char *json, size_t len, control_config_t *out,
                              const char **detail);

typedef enum {
    CONTROL_VERDICT_EXECUTE = 0, /* publicar `received` e executar `action` */
    CONTROL_VERDICT_REJECT,      /* publicar `rejected` com `entry->reason` */
    CONTROL_VERDICT_DUPLICATE,   /* id repetido: reenviar o último ACK de `entry`, sem agir */
} control_verdict_kind_t;

typedef enum {
    CONTROL_ACTION_NONE = 0,
    CONTROL_ACTION_PULSE,
    CONTROL_ACTION_ESTOP,
} control_action_kind_t;

typedef struct {
    char id[CONTRACT_COMMAND_ID_MAX_LEN + 1U];
    bool used;
    contract_cmd_status_t last_status; /* último ACK publicado para este id */
    char reason[CONTRACT_REASON_MAX_LEN + 1U];
} control_entry_t;

typedef struct {
    control_verdict_kind_t kind;
    control_action_kind_t action;
    uint8_t channel;
    uint32_t pulse_ms; /* já limitado ao canal e ao teto do firmware */
    const control_entry_t *entry;
} control_verdict_t;

/** @brief Estado do ambiente no instante em que o comando chegou. */
typedef struct {
    bool time_valid;
    int64_t now_epoch;
    uint8_t relay_count;  /* relés instalados nesta placa */
    uint16_t busy_mask;   /* canais com pulso em andamento */
    bool control_disabled; /* firmware sem acionamento: recusa todo comando válido */
} control_env_t;

typedef struct {
    bool has_config;
    control_config_t config;
    bool estop;
    uint32_t estop_at_version; /* versão da config em vigor quando o E-stop travou */
    control_entry_t cache[CONTRACT_COMMAND_CACHE_SIZE];
    uint8_t cache_next;
} control_ctx_t;

/**
 * @brief Começa sem config (comandos recusados com `no_config`).
 *
 * @param estop_latched    E-stop persistido de um boot anterior
 * @param estop_at_version Versão da config quando ele travou
 */
void control_init(control_ctx_t *ctx, bool estop_latched, uint32_t estop_at_version);

/**
 * @brief Aplica uma config válida. Nunca fecha relé.
 *
 * `estop: true` trava. `estop: false` só destrava com `version` maior que a
 * do momento em que travou: a config retida volta igual a cada reconexão e
 * não pode desfazer sozinha um E-stop dado por comando.
 *
 * @return true se o estado do E-stop mudou (persistir)
 */
bool control_apply_config(control_ctx_t *ctx, const control_config_t *config);

/** @brief Decide o destino de um comando recém-chegado. */
void control_evaluate(control_ctx_t *ctx, const contract_command_t *cmd, const control_env_t *env,
                      control_verdict_t *verdict);

/**
 * @brief Registra o ACK publicado para um id (para repetir igual em duplicata).
 *
 * @return A entrada, ou NULL se o id não está no cache
 */
const control_entry_t *control_record(control_ctx_t *ctx, const char *id,
                                      contract_cmd_status_t status, const char *reason);
