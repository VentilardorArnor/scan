#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "app_bus.h"
#include "app_tasks.h"
#include "contract_json.h"
#include "device_state.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "identity.h"
#include "mqtt_link.h"
#include "sdkconfig.h"
#include "timekeeping.h"
#include "wifi_sta.h"

/*
 * mqtt_task — dona da conexão com o broker: espera rede, hora e identidade,
 * conecta, reconecta com backoff exponencial e jitter, publica o que as outras
 * tarefas enfileiram e distribui o que chega.
 */

#define MQTT_TICK_MS          (200U)
#define PREREQ_LOG_PERIOD_MS  (30000U)
#define IDENTITY_POLL_MS      (2000U) /* o enroll pode terminar a qualquer momento */
#define URI_MAX               (300U)
#define BACKOFF_START_MS      (1000U)
#define BACKOFF_MAX_MS        (CONTRACT_BACKOFF_MAX_MS) /* teto de 5 min */
#define EVENTS_PER_TICK       (4U)
#define EVENT_BUF_LEN         (512U)
#define MS_PER_S              (1000LL)

static const char *TAG = "mqtt_task";

static identity_t s_identity;
static char s_uri[URI_MAX];
static atomic_bool s_conn_changed;
static char s_event_buf[EVENT_BUF_LEN];

/* ---- callbacks: rodam na tarefa do cliente MQTT, só enfileiram ---- */

static void on_rx(mqtt_link_topic_t topic, const char *data, size_t len)
{
    bool delivered = false;
    switch (topic) {
    case MQTT_LINK_TOPIC_TELEMETRY_ACK:
        delivered = journal_task_post_ack(data, len);
        break;
    case MQTT_LINK_TOPIC_CMD:
        delivered = cmd_task_post_json(false, data, len);
        break;
    case MQTT_LINK_TOPIC_CONFIG:
        delivered = cmd_task_post_json(true, data, len);
        break;
    default:
        break;
    }
    if (!delivered) {
        ESP_LOGW(TAG, "mensagem recebida descartada (fila cheia ou grande demais)");
    }
}

static void on_conn(bool connected)
{
    (void)connected;
    atomic_store(&s_conn_changed, true);
}

/* ---- publicação ---- */

/* Eventos guardam o instante monotônico do fato; o `ts` é calculado aqui, na
 * hora de publicar, quando o relógio já está certo. Assim um evento do boot
 * (antes do NTP) sai com a hora em que realmente aconteceu. */
static void publish_event(const app_bus_event_t *event)
{
    const uint64_t age_ms = timekeeping_mono_ms() - event->at_ms;
    const int64_t ts = timekeeping_now() - (int64_t)(age_ms / (uint64_t)MS_PER_S);

    if (contract_build_event(ts, event->type, event->data, s_event_buf, sizeof(s_event_buf)) !=
        FW_OK) {
        ESP_LOGW(TAG, "evento %s descartado (data inválido)", event->type);
        return;
    }
    (void)mqtt_link_publish(MQTT_LINK_TOPIC_EVENT, s_event_buf, strlen(s_event_buf), false);
}

static void drain_outbound(void)
{
    static app_bus_pub_t pub;
    app_bus_event_t event;

    /* Publicações comuns não ficam guardadas offline: a telemetria é
     * reenviada a partir do journal, e status/state são republicados ao
     * conectar. Só os eventos esperam na fila deles. */
    if (app_bus_take_publish(&pub, MQTT_TICK_MS) && mqtt_link_is_connected()) {
        if (mqtt_link_publish(pub.topic, pub.payload, pub.len, pub.retain) != FW_OK) {
            ESP_LOGW(TAG, "publicação recusada pelo cliente MQTT");
        }
    }

    for (uint32_t i = 0; (i < EVENTS_PER_TICK) && mqtt_link_is_connected(); i++) {
        if (!app_bus_take_event(&event)) {
            break;
        }
        publish_event(&event);
    }
}

/* ---- conexão ---- */

static uint32_t next_backoff(uint32_t *backoff_ms)
{
    /* Metade fixa + metade aleatória: espalha a volta de vários dispositivos
     * depois de uma queda do broker. */
    const uint32_t half = *backoff_ms / 2U;
    const uint32_t delay = half + (esp_random() % (half + 1U));
    *backoff_ms = (*backoff_ms >= (BACKOFF_MAX_MS / 2U)) ? BACKOFF_MAX_MS : (*backoff_ms * 2U);
    return delay;
}

/* Rede, hora válida (o TLS confere a validade do certificado) e identidade. */
static bool prerequisites_ready(bool *identity_loaded, uint64_t now_ms, uint64_t *next_log_ms)
{
    const char *missing = NULL;

    static uint64_t s_next_identity_ms;
    if (!*identity_loaded) {
        if (now_ms >= s_next_identity_ms) {
            *identity_loaded = (identity_load(&s_identity) == FW_OK);
            s_next_identity_ms = now_ms + IDENTITY_POLL_MS;
        }
        if (!*identity_loaded) {
            missing = "identidade (certificado do enroll)";
        }
    }
    if ((missing == NULL) && !wifi_sta_has_ip()) {
        missing = "Wi-Fi";
    }
    if ((missing == NULL) && !timekeeping_is_valid()) {
        missing = "hora válida (NTP ou GPS)";
    }

    if ((missing != NULL) && (now_ms >= *next_log_ms)) {
        ESP_LOGW(TAG, "aguardando: %s", missing);
        *next_log_ms = now_ms + PREREQ_LOG_PERIOD_MS;
    }
    return missing == NULL;
}

static void mqtt_task(void *arg)
{
    (void)arg;
    bool identity_loaded = false;
    bool started = false;
    bool was_connected = false;
    bool reconnect_pending = false;
    uint32_t backoff_ms = BACKOFF_START_MS;
    uint64_t reconnect_at_ms = 0;
    uint64_t next_log_ms = 0;

    (void)esp_task_wdt_add(NULL);

    for (;;) {
        const uint64_t now_ms = timekeeping_mono_ms();

        if (!started) {
            if (prerequisites_ready(&identity_loaded, now_ms, &next_log_ms)) {
                /* Broker informado pelo enroll (mqtt.host / mqtt.port). */
                (void)snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%u", s_identity.mqtt_host,
                               (unsigned)s_identity.mqtt_port);
                const mqtt_link_config_t config = {
                    .uri = s_uri,
                    .identity = &s_identity,
                    /* Com relés, a placa entra no protocolo de controle mesmo
                     * sem acionamento: recusa em vez de deixar o comando expirar. */
                    .control_topics = (BOARD_HAS_RELAYS != 0),
                    .on_rx = on_rx,
                    .on_conn = on_conn,
                };
                if ((mqtt_link_init(&config) == FW_OK) && (mqtt_link_start() == FW_OK)) {
                    started = true;
                    device_state_set(DEVICE_STATE_MQTT_CONNECTING);
                    ESP_LOGI(TAG, "conectando a %s como %s", s_uri, s_identity.device_id);
                } else {
                    ESP_LOGE(TAG, "falha ao criar o cliente MQTT");
                    next_log_ms = now_ms + PREREQ_LOG_PERIOD_MS;
                }
            }
        } else {
            const bool connected = mqtt_link_is_connected();
            const bool changed = atomic_exchange(&s_conn_changed, false);

            if (connected && !was_connected) {
                backoff_ms = BACKOFF_START_MS;
                reconnect_pending = false;
                device_state_set(DEVICE_STATE_ONLINE);
                /* Cada tarefa republica o que é dela: status, state, pendentes. */
                health_task_notify_connected();
                journal_task_notify_connected();
            } else if (!connected && (changed || was_connected) && !reconnect_pending) {
                if (wifi_sta_has_ip()) {
                    device_state_set(DEVICE_STATE_MQTT_CONNECTING); /* sem IP, o Wi-Fi informa */
                }
                const uint32_t delay_ms = next_backoff(&backoff_ms);
                reconnect_at_ms = now_ms + delay_ms;
                reconnect_pending = true;
                ESP_LOGW(TAG, "sem broker; nova tentativa em %u ms", (unsigned)delay_ms);
            } else {
                /* estado inalterado */
            }

            if (!connected && wifi_sta_has_ip() &&
                (device_state_get() != DEVICE_STATE_MQTT_CONNECTING)) {
                device_state_set(DEVICE_STATE_MQTT_CONNECTING); /* rede voltou, broker ainda não */
            }
            if (!connected && reconnect_pending && (now_ms >= reconnect_at_ms)) {
                reconnect_pending = false;
                if (mqtt_link_reconnect() != FW_OK) {
                    atomic_store(&s_conn_changed, true); /* agenda outra tentativa */
                }
            }
            mqtt_link_poll(); /* SUBACK que não veio ou foi recusado: assina de novo */
            was_connected = connected;
        }

        drain_outbound(); /* bloqueia até MQTT_TICK_MS esperando publicações */
        (void)esp_task_wdt_reset();
    }
}

void mqtt_task_start(void)
{
    const BaseType_t created =
        xTaskCreate(mqtt_task, "mqtt_task", APP_TASK_STACK_LARGE, NULL, APP_TASK_PRIO_MQTT, NULL);
    configASSERT(created == pdPASS);
}
