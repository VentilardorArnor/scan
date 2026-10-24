#include <stdio.h>
#include <string.h>

#include "app_bus.h"
#include "app_tasks.h"

/*
 * cmd_task — aplica a `config` retida e decide cada `cmd` (docs/MQTT.md
 * §Comandos). O que for aprovado vai para a relay_task. Existe em toda placa
 * com relés; sem PIVO_REMOTE_CONTROL a config vale normalmente, mas todo
 * comando válido é recusado com `device_control_disabled`.
 */

#if BOARD_HAS_RELAYS

#include "control.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "relay_hw.h"
#include "timekeeping.h"

#define CMD_QUEUE_LEN     (4U)
#define CMD_TICK_MS       (500U)
#define LOCK_TIMEOUT_MS   (200U)
#define CMD_JSON_MAX      (2048U)
#define ACK_BUF_LEN       (256U)

#define NVS_NAMESPACE     "ctl"
#define NVS_KEY_ESTOP     "estop"
#define NVS_KEY_ESTOP_VER "estop_ver"

typedef enum {
    CMD_MSG_COMMAND = 0,
    CMD_MSG_CONFIG,
    CMD_MSG_RESULT,
} cmd_msg_kind_t;

typedef struct {
    cmd_msg_kind_t kind;
    uint16_t len;
    char json[CMD_JSON_MAX + 1U];                 /* COMMAND / CONFIG */
    char id[CONTRACT_COMMAND_ID_MAX_LEN + 1U];    /* RESULT */
    bool ok;
    char reason[CONTRACT_REASON_MAX_LEN + 1U];
} cmd_msg_t;

static const char *TAG = "cmd_task";

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_post_lock;
static cmd_msg_t s_post_scratch; /* protegido por s_post_lock */
static cmd_msg_t s_msg;          /* só a cmd_task usa */
static control_ctx_t s_ctx;      /* só a cmd_task usa */
static char s_ack_buf[ACK_BUF_LEN];

/* ---------------- E-stop persistido ---------------- */

static void persist_estop(void)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS indisponível: E-stop não persistido");
        return;
    }
    if ((nvs_set_u8(handle, NVS_KEY_ESTOP, s_ctx.estop ? 1U : 0U) != ESP_OK) ||
        (nvs_set_u32(handle, NVS_KEY_ESTOP_VER, s_ctx.estop_at_version) != ESP_OK) ||
        (nvs_commit(handle) != ESP_OK)) {
        ESP_LOGE(TAG, "falha ao persistir o E-stop");
    }
    nvs_close(handle);
}

static void load_estop(bool *estop, uint32_t *version)
{
    nvs_handle_t handle = 0;
    uint8_t flag = 0;
    *estop = false;
    *version = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    if (nvs_get_u8(handle, NVS_KEY_ESTOP, &flag) == ESP_OK) {
        *estop = (flag != 0U);
    }
    (void)nvs_get_u32(handle, NVS_KEY_ESTOP_VER, version);
    nvs_close(handle);
}

/* ---------------- saídas ---------------- */

static void publish_ack(const char *id, contract_cmd_status_t status, const char *reason)
{
    const bool time_ok = timekeeping_is_valid();
    const contract_cmd_ack_t ack = {
        .id = id,
        .status = status,
        .has_ts = time_ok,
        .ts = time_ok ? timekeeping_now() : 0,
        .reason = reason,
    };
    if ((contract_build_cmd_ack(&ack, s_ack_buf, sizeof(s_ack_buf)) != FW_OK) ||
        !app_bus_publish(MQTT_LINK_TOPIC_CMD_ACK, false, s_ack_buf, strlen(s_ack_buf))) {
        ESP_LOGW(TAG, "ACK de %s não publicado", id);
        return;
    }
    ESP_LOGI(TAG, "cmd/ack enfileirado: %s", s_ack_buf);
}

static void send_meta(void)
{
    const relay_request_t request = {
        .op = RELAY_OP_META,
        .config_version = s_ctx.has_config ? s_ctx.config.version : 0U,
        .estop = s_ctx.estop,
    };
    (void)relay_task_request(&request);
}

/* ---------------- comando ---------------- */

static void handle_command(const char *json, size_t len)
{
    contract_command_t cmd;
    control_verdict_t verdict;

    if (contract_parse_command(json, len, &cmd) != FW_OK) {
        ESP_LOGW(TAG, "comando ilegível descartado (sem id não há como responder)");
        return;
    }

    const control_env_t env = {
        .time_valid = timekeeping_is_valid(),
        .now_epoch = timekeeping_now(),
        .relay_count = relay_hw_get_relay_count(),
        .busy_mask = relay_task_busy_mask(),
        .control_disabled = (REMOTE_CONTROL_ENABLED == 0),
    };
    control_evaluate(&s_ctx, &cmd, &env, &verdict);

    switch (verdict.kind) {
    case CONTROL_VERDICT_DUPLICATE:
        /* Reenvio do servidor (não viu o `received`): repete o último ACK
         * deste id, sem agir de novo. */
        ESP_LOGI(TAG, "comando %s repetido: reenviado o último ACK", cmd.id);
        publish_ack(cmd.id, verdict.entry->last_status, verdict.entry->reason);
        return;

    case CONTROL_VERDICT_REJECT:
        ESP_LOGW(TAG, "comando %s recusado: %s", cmd.id, verdict.entry->reason);
        publish_ack(cmd.id, CONTRACT_CMD_STATUS_REJECTED, verdict.entry->reason);
        return;

    case CONTROL_VERDICT_EXECUTE:
    default:
        break;
    }
#if !REMOTE_CONTROL_ENABLED
    /* Inalcançável (control_evaluate recusa antes); nada vai à relay_task. */
    publish_ack(cmd.id, CONTRACT_CMD_STATUS_REJECTED, CONTRACT_CMD_REASON_CONTROL_DISABLED);
    return;
#endif

    relay_request_t request;
    memset(&request, 0, sizeof(request));
    (void)snprintf(request.id, sizeof(request.id), "%s", cmd.id);

    if (verdict.action == CONTROL_ACTION_ESTOP) {
        persist_estop(); /* antes de abrir: se reiniciar no meio, volta travado */
        request.op = RELAY_OP_ESTOP;
    } else {
        request.op = RELAY_OP_PULSE;
        request.channel = verdict.channel;
        request.ms = verdict.pulse_ms;
    }

    if (relay_task_request(&request)) {
        publish_ack(cmd.id, CONTRACT_CMD_STATUS_RECEIVED, NULL);
    } else {
        (void)control_record(&s_ctx, cmd.id, CONTRACT_CMD_STATUS_FAILED, CONTRACT_CMD_REASON_BUSY);
        publish_ack(cmd.id, CONTRACT_CMD_STATUS_FAILED, CONTRACT_CMD_REASON_BUSY);
    }
}

static void handle_result(const cmd_msg_t *msg)
{
    const contract_cmd_status_t status =
        msg->ok ? CONTRACT_CMD_STATUS_COMPLETED : CONTRACT_CMD_STATUS_FAILED;
    const char *reason = msg->ok ? NULL : msg->reason;
    (void)control_record(&s_ctx, msg->id, status, reason);
    publish_ack(msg->id, status, reason);
}

/* ---------------- config ---------------- */

static void handle_config(const char *json, size_t len)
{
    control_config_t config;
    const char *detail = "";

    if (control_config_parse(json, len, &config, &detail) != FW_OK) {
        ESP_LOGW(TAG, "config inválida (%s); mantida a anterior", detail);
        app_bus_post_event(CONTRACT_EV_CONFIG_INVALID, "{\"detail\":\"%s\"}", detail);
        return;
    }

    if (control_apply_config(&s_ctx, &config)) {
        persist_estop();
    }
    app_bus_set_failsafe_s(s_ctx.config.failsafe_s);

    if (s_ctx.estop) {
        /* E-stop em vigor: garante tudo aberto (idempotente). */
        const relay_request_t request = {.op = RELAY_OP_ESTOP};
        (void)relay_task_request(&request);
    }
    send_meta(); /* state com a nova config_version: libera os botões no servidor */
    ESP_LOGI(TAG, "config v%u aplicada (%u canais, estop=%d, failsafe=%u s)",
             (unsigned)config.version, (unsigned)config.channel_count, (int)s_ctx.estop,
             (unsigned)config.failsafe_s);
    const uint8_t relay_count = relay_hw_get_relay_count();
    for (uint8_t i = 0; i < config.channel_count; i++) {
        const control_channel_t *ch = &config.channels[i];
        ESP_LOGI(TAG, "  canal %u: tipo %d, pulso máx %u ms -> %s", (unsigned)ch->channel,
                 (int)ch->kind, (unsigned)ch->pulse_ms,
                 (ch->channel < relay_count) ? "relé instalado" : "SEM RELÉ (recusado)");
    }
}

/* ---------------- tarefa ---------------- */

static void cmd_task(void *arg)
{
    (void)arg;
    bool estop = false;
    uint32_t estop_version = 0;

    load_estop(&estop, &estop_version);
    control_init(&s_ctx, estop, estop_version);
    if (s_ctx.estop) {
        ESP_LOGW(TAG, "E-stop travado de um boot anterior");
    }
    send_meta();

    (void)esp_task_wdt_add(NULL);

    for (;;) {
        if (xQueueReceive(s_queue, &s_msg, pdMS_TO_TICKS(CMD_TICK_MS)) == pdTRUE) {
            switch (s_msg.kind) {
            case CMD_MSG_COMMAND:
                handle_command(s_msg.json, s_msg.len);
                break;
            case CMD_MSG_CONFIG:
                handle_config(s_msg.json, s_msg.len);
                break;
            case CMD_MSG_RESULT:
                handle_result(&s_msg);
                break;
            default:
                break;
            }
        }
        (void)esp_task_wdt_reset();
    }
}

void cmd_task_start(void)
{
    s_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_msg_t));
    s_post_lock = xSemaphoreCreateMutex();
    configASSERT((s_queue != NULL) && (s_post_lock != NULL));
    const BaseType_t created =
        xTaskCreate(cmd_task, "cmd_task", APP_TASK_STACK_LARGE, NULL, APP_TASK_PRIO_CMD, NULL);
    configASSERT(created == pdPASS);
}

/* Chamada na tarefa do cliente MQTT: copia e enfileira, nada mais. */
bool cmd_task_post_json(bool is_config, const char *json, size_t len)
{
    if ((json == NULL) || (s_queue == NULL)) {
        return false;
    }
    if (len > CMD_JSON_MAX) {
        if (is_config) {
            app_bus_post_event(CONTRACT_EV_CONFIG_INVALID, "{\"detail\":\"too_large\"}");
        }
        return false;
    }
    if (xSemaphoreTake(s_post_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    s_post_scratch.kind = is_config ? CMD_MSG_CONFIG : CMD_MSG_COMMAND;
    s_post_scratch.len = (uint16_t)len;
    memcpy(s_post_scratch.json, json, len);
    s_post_scratch.json[len] = '\0';
    const bool queued = (xQueueSend(s_queue, &s_post_scratch, 0) == pdTRUE);
    (void)xSemaphoreGive(s_post_lock);
    return queued;
}

bool cmd_task_post_result(const char *id, bool ok, const char *reason)
{
    if ((id == NULL) || (s_queue == NULL)) {
        return false;
    }
    if (xSemaphoreTake(s_post_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    s_post_scratch.kind = CMD_MSG_RESULT;
    s_post_scratch.len = 0;
    s_post_scratch.json[0] = '\0';
    (void)snprintf(s_post_scratch.id, sizeof(s_post_scratch.id), "%s", id);
    s_post_scratch.ok = ok;
    (void)snprintf(s_post_scratch.reason, sizeof(s_post_scratch.reason), "%s",
                   (reason != NULL) ? reason : "");
    const bool queued = (xQueueSend(s_queue, &s_post_scratch, 0) == pdTRUE);
    (void)xSemaphoreGive(s_post_lock);
    return queued;
}

#else /* !BOARD_HAS_RELAYS */

/* Placa sem relés: cmd/config não são assinados. */

void cmd_task_start(void)
{
}

bool cmd_task_post_json(bool is_config, const char *json, size_t len)
{
    (void)is_config;
    (void)json;
    (void)len;
    return false;
}

bool cmd_task_post_result(const char *id, bool ok, const char *reason)
{
    (void)id;
    (void)ok;
    (void)reason;
    return false;
}

#endif
