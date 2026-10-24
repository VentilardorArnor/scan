#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "app_bus.h"
#include "app_tasks.h"
#include "contract_json.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_link.h"
#include "relay_hw.h"
#include "timekeeping.h"

/*
 * relay_task — única dona dos relés (ADR-007).
 *
 * Sem PIVO_REMOTE_CONTROL só publica o `state` (relés abertos, versão da
 * config aplicada) e mantém o fail-safe; os pinos só recebem o nível inativo.
 *
 * Executa os pulsos e o E-stop aprovados pela cmd_task, aplica o fail-safe
 * e publica o `state` ao mudar e a cada 10 s. Nunca fecha um relé por
 * iniciativa própria e nunca reaplica nada de antes de um reboot.
 */

#define RELAY_QUEUE_LEN    (8U)
#define RELAY_TICK_MS      (100U)
#define STATE_BUF_LEN      (384U)
#define MS_PER_S           (1000ULL)
#define CH_BIT(n)          ((uint16_t)(1U << (n)))

static const char *TAG = "relay_task";

static QueueHandle_t s_queue;
static atomic_uint_fast16_t s_busy_mask;
static char s_pulse_id[RELAY_HW_MAX_CHANNELS][CONTRACT_COMMAND_ID_MAX_LEN + 1U];
static char s_state_buf[STATE_BUF_LEN];
static uint32_t s_config_version;
static bool s_estop;
static bool s_failsafe_active;
static uint64_t s_next_state_ms;

static void publish_state(void)
{
    if (!timekeeping_is_valid() || !mqtt_link_is_connected()) {
        return; /* sem hora não há `ts`; sem broker, o próximo sai ao conectar */
    }
    const uint8_t on_mask = relay_hw_get_on_mask();
    const contract_state_t state = {
        .ts = timekeeping_now(),
        .config_version = s_config_version,
        .estop = s_estop,
        .relay_count = relay_hw_get_relay_count(),
        /* O que foi realmente escrito no GPIO, nunca o pedido. */
        .relays_mask = (uint16_t)on_mask,
        .input_count = relay_hw_get_input_count(),
        .inputs_mask = (uint16_t)relay_hw_read_inputs_mask(),
    };
    if (contract_build_state(&state, s_state_buf, sizeof(s_state_buf)) == FW_OK) {
        (void)app_bus_publish(MQTT_LINK_TOPIC_STATE, false, s_state_buf, strlen(s_state_buf));
    }
    s_next_state_ms = timekeeping_mono_ms() + (CONTRACT_STATE_INTERVAL_S * MS_PER_S);
}

static void report(const char *id, bool ok, const char *reason)
{
    if (id[0] == '\0') {
        return; /* ação da config ou do fail-safe: ninguém espera ACK */
    }
    if (!cmd_task_post_result(id, ok, reason)) {
        ESP_LOGW(TAG, "resultado de %s não entregue à cmd_task", id);
    }
}

static void clear_pulse(uint8_t index)
{
    s_pulse_id[index][0] = '\0';
    (void)atomic_fetch_and(&s_busy_mask, (uint_fast16_t)~CH_BIT(index));
}

/* Abre tudo; pulsos interrompidos terminam como `failed`. */
static void open_all(const char *reason)
{
    relay_hw_all_off();
    for (uint8_t i = 0; i < RELAY_HW_MAX_CHANNELS; i++) {
        if (s_pulse_id[i][0] != '\0') {
            report(s_pulse_id[i], false, reason);
            clear_pulse(i);
        }
    }
}

static void handle_request(const relay_request_t *req)
{
    const uint8_t index = req->channel;

    switch (req->op) {
    case RELAY_OP_PULSE:
        if (REMOTE_CONTROL_ENABLED == 0) {
            /* Sem acionamento o pulso nunca chega ao GPIO, venha de onde vier. */
            report(req->id, false, CONTRACT_CMD_REASON_CONTROL_DISABLED);
            break;
        }
        if ((index >= relay_hw_get_relay_count()) || s_estop) {
            report(req->id, false, s_estop ? CONTRACT_CMD_REASON_ESTOP : CONTRACT_CMD_REASON_HARDWARE);
            break;
        }
        (void)snprintf(s_pulse_id[index], sizeof(s_pulse_id[index]), "%s", req->id);
        (void)atomic_fetch_or(&s_busy_mask, CH_BIT(index));
        if (relay_hw_pulse_start(index, req->ms) == FW_OK) {
            ESP_LOGI(TAG, "pulso de %u ms no canal %u", (unsigned)req->ms, (unsigned)index);
            publish_state();
        } else {
            clear_pulse(index);
            report(req->id, false, CONTRACT_CMD_REASON_HARDWARE);
        }
        break;

    case RELAY_OP_PULSE_DONE:
        /* O relay_hw já abriu o relé; aqui só fecha o comando. */
        ESP_LOGI(TAG, "fim do pulso no canal %u: saída %s", (unsigned)index,
                 ((relay_hw_get_on_mask() & CH_BIT(index)) == 0U) ? "desligada" : "AINDA LIGADA");
        if ((index < RELAY_HW_MAX_CHANNELS) && (s_pulse_id[index][0] != '\0')) {
            report(s_pulse_id[index], true, NULL);
            clear_pulse(index);
        }
        publish_state();
        break;

    case RELAY_OP_ESTOP: {
        /* A config com estop:true volta a cada reconexão: o evento só sai
         * quando o E-stop começa ou quando vem de um comando. */
        const bool already = s_estop;
        open_all(CONTRACT_CMD_REASON_ESTOP);
        s_estop = true;
        if (!already || (req->id[0] != '\0')) {
            ESP_LOGW(TAG, "E-stop: todos os relés abertos");
            app_bus_post_event(CONTRACT_EV_RELAY_ESTOP, "{\"source\":\"%s\"}",
                               (req->id[0] != '\0') ? "command" : "config");
        }
        report(req->id, true, NULL);
        publish_state();
        break;
    }

    case RELAY_OP_META:
        if ((req->config_version != s_config_version) || (req->estop != s_estop)) {
            ESP_LOGI(TAG, "state passa a confirmar config v%u (estop=%d, relés %s)",
                     (unsigned)req->config_version, (int)req->estop,
                     (relay_hw_get_on_mask() == 0U) ? "todos abertos" : "COM RELÉ FECHADO");
        }
        s_config_version = req->config_version;
        s_estop = req->estop;
        publish_state(); /* confirma a versão aplicada para o servidor liberar os botões */
        break;

    case RELAY_OP_PUBLISH_STATE:
        publish_state();
        break;

    default:
        break;
    }
}

/* Sem broker por mais de failsafe_s: tudo aberto. Usa só o indicador atômico
 * do cliente MQTT, então funciona mesmo com a mqtt_task travada. */
static void check_failsafe(uint64_t now_ms, uint64_t *last_connected_ms, bool *was_connected)
{
    const bool connected = mqtt_link_is_connected();

    if (connected) {
        *last_connected_ms = now_ms;
        if (!*was_connected) {
            s_failsafe_active = false;
            publish_state(); /* informa o estado real e NÃO fecha nada */
        }
    } else if (!s_failsafe_active) {
        const uint32_t failsafe_s = app_bus_failsafe_s();
        if ((now_ms - *last_connected_ms) > ((uint64_t)failsafe_s * MS_PER_S)) {
            open_all(CONTRACT_CMD_REASON_FAILSAFE);
            s_failsafe_active = true;
            ESP_LOGW(TAG, "fail-safe: %u s sem broker, relés abertos", (unsigned)failsafe_s);
            app_bus_post_event(CONTRACT_EV_RELAY_FAILSAFE,
                               "{\"reason\":\"broker_lost\",\"after_s\":%u}", (unsigned)failsafe_s);
        }
    } else {
        /* fail-safe já aplicado; aguarda a reconexão */
    }
    *was_connected = connected;
}

static void relay_task(void *arg)
{
    (void)arg;
    relay_request_t request;
    /* Conta a partir do boot: sem nunca conectar, o fail-safe também dispara. */
    uint64_t last_connected_ms = timekeeping_mono_ms();
    bool was_connected = false;

    (void)esp_task_wdt_add(NULL);

    for (;;) {
        if (xQueueReceive(s_queue, &request, pdMS_TO_TICKS(RELAY_TICK_MS)) == pdTRUE) {
            handle_request(&request);
        }
        const uint64_t now_ms = timekeeping_mono_ms();
        check_failsafe(now_ms, &last_connected_ms, &was_connected);
        if (now_ms >= s_next_state_ms) {
            publish_state(); /* a cada 10 s; o servidor bloqueia botões com state velho */
            s_next_state_ms = now_ms + (CONTRACT_STATE_INTERVAL_S * MS_PER_S);
        }
        (void)esp_task_wdt_reset();
    }
}

void relay_task_start(void)
{
    s_queue = xQueueCreate(RELAY_QUEUE_LEN, sizeof(relay_request_t));
    configASSERT(s_queue != NULL);
    const BaseType_t created = xTaskCreate(relay_task, "relay_task", APP_TASK_STACK_DEFAULT, NULL,
                                           APP_TASK_PRIO_RELAY, NULL);
    configASSERT(created == pdPASS);
}

bool relay_task_request(const relay_request_t *request)
{
    if ((request == NULL) || (s_queue == NULL)) {
        return false;
    }
    if (request->op == RELAY_OP_ESTOP) {
        /* E-stop fura a fila: é atendido antes de qualquer pedido pendente. */
        return xQueueSendToFront(s_queue, request, 0) == pdTRUE;
    }
    return xQueueSend(s_queue, request, 0) == pdTRUE;
}

uint16_t relay_task_busy_mask(void)
{
    return (uint16_t)atomic_load(&s_busy_mask);
}

void relay_task_on_pulse_done(uint8_t index)
{
    /* Roda na tarefa do esp_timer; o relé JÁ foi aberto pelo relay_hw. */
    if (s_queue == NULL) {
        return;
    }
    const relay_request_t request = {
        .op = RELAY_OP_PULSE_DONE,
        .channel = index,
    };
    (void)xQueueSend(s_queue, &request, 0);
}
