#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "contract_json.h"
#include "sdkconfig.h"

/* Placa com relés (menuconfig -> pivo-fw): os pinos são configurados e
 * mantidos abertos. */
#ifdef CONFIG_PIVO_BOARD_HAS_RELAYS
#define BOARD_HAS_RELAYS (1)
#else
#define BOARD_HAS_RELAYS (0)
#endif

/* Acionamento pelo painel web (menuconfig -> pivo-fw). Desligado, a placa com
 * relés continua no protocolo (assina cmd/config, publica state e cmd/ack),
 * mas recusa todo comando com `device_control_disabled` e nunca fecha relé. */
#if defined(CONFIG_PIVO_REMOTE_CONTROL) && BOARD_HAS_RELAYS
#define REMOTE_CONTROL_ENABLED (1)
#else
#define REMOTE_CONTROL_ENABLED (0)
#endif

/*
 * Tarefas do firmware. Cada recurso tem uma única dona; o resto fala com ela
 * por fila.
 *
 * | Tarefa       | Prio | Pilha | Período      | Dona de                          |
 * |--------------|------|-------|--------------|----------------------------------|
 * | relay_task   | 10   | 4096  | 100 ms       | relés, pulsos, fail-safe, state  |
 * | cmd_task     | 7    | 6144  | por evento   | config, E-stop, ids executados   |
 * | provision    | 4    | 8192  | até o enroll | chave, CSR, POST /enroll         |
 * | gps_task     | 6    | 4096  | UART (200 ms)| UART do GPS, filtro de posição   |
 * | mqtt_task    | 6    | 6144  | 200 ms       | conexão e publicações            |
 * | journal_task | 5    | 6144  | 500 ms       | journal (LittleFS), telemetria   |
 * | health_task  | 3    | 4096  | 1 s          | heartbeat de status              |
 *
 * Todas, menos a provision (que espera a rede), estão no task watchdog (10 s).
 */

#define APP_TASK_PRIO_RELAY    (10U)
#define APP_TASK_PRIO_CMD      (7U)
#define APP_TASK_PRIO_GPS      (6U)
#define APP_TASK_PRIO_MQTT     (6U)
#define APP_TASK_PRIO_JOURNAL  (5U)
#define APP_TASK_PRIO_PROVISION (4U)
#define APP_TASK_PRIO_HEALTH   (3U)

#define APP_TASK_STACK_DEFAULT (4096U)
#define APP_TASK_STACK_LARGE   (6144U)
#define APP_TASK_STACK_TLS     (8192U)

/* ---------------- relay_task ---------------- */

typedef enum {
    RELAY_OP_PULSE = 0,
    RELAY_OP_ESTOP,         /* abre tudo; com id = comando, sem id = config */
    RELAY_OP_PULSE_DONE,    /* interno: o esp_timer encerrou um pulso */
    RELAY_OP_META,          /* config_version e estop para o state */
    RELAY_OP_PUBLISH_STATE,
} relay_op_t;

typedef struct {
    relay_op_t op;
    uint8_t channel; /* 0..15 (= índice do relé) */
    uint32_t ms;
    char id[CONTRACT_COMMAND_ID_MAX_LEN + 1U]; /* "" = sem comando */
    uint32_t config_version;
    bool estop;
} relay_request_t;

void relay_task_start(void);
bool relay_task_request(const relay_request_t *request);
/* Canais com pulso em andamento (bit n = canal n). */
uint16_t relay_task_busy_mask(void);
/* Callback de fim de pulso para relay_hw_init(); roda na tarefa do esp_timer. */
void relay_task_on_pulse_done(uint8_t index);

/* ---------------- cmd_task ---------------- */

void cmd_task_start(void);
bool cmd_task_post_json(bool is_config, const char *json, size_t len);
bool cmd_task_post_result(const char *id, bool ok, const char *reason);

/* ---------------- provision_task ---------------- */

/* Faz o enroll se ainda não há certificado; termina sozinha depois. */
void provision_task_start(void);

/* ---------------- gps_task ---------------- */

void gps_task_start(void);
/* Posição filtrada atual; false = sem fix (omitir o objeto gps). */
bool gps_task_get_fix(contract_gps_t *out);

/* ---------------- journal_task ---------------- */

void journal_task_start(void);
bool journal_task_post_ack(const char *json, size_t len);
void journal_task_notify_connected(void);
/* false se o journal não pôde ser montado/aberto (entra no autoteste). */
bool journal_task_is_healthy(void);

/* Pede a formatação do journal no próximo boot (reset de fábrica). */
void journal_task_request_wipe(void);

/* ---------------- button_task ---------------- */

/* Troca de Wi-Fi e reset de fábrica pelo botão BOOT. */
void button_task_start(void);

/* ---------------- mqtt_task / health_task ---------------- */

void mqtt_task_start(void);
/* `hardware_ok`: resultado da inicialização dos relés, usado no autoteste. */
void health_task_start(bool hardware_ok);
void health_task_notify_connected(void);
