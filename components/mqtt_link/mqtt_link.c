#include "mqtt_link.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "contract.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#define NETWORK_TIMEOUT_MS (10000)
#define RX_BUFFER_BYTES    (4096)
#define TX_BUFFER_BYTES    (2048)
#define SUBACK_TIMEOUT_MS  (10000U)
#define SUB_RETRY_MS       (60000U) /* depois de uma recusa (SUBACK 0x80) */
#define SUBACK_FAILURE     (0x80U)

static const char *TAG = "mqtt_link";

static const char *const TOPIC_SUFFIX[MQTT_LINK_TOPIC_COUNT] = {
    [MQTT_LINK_TOPIC_TELEMETRY] = CONTRACT_TOPIC_TELEMETRY,
    [MQTT_LINK_TOPIC_TELEMETRY_ACK] = CONTRACT_TOPIC_TELEMETRY_ACK,
    [MQTT_LINK_TOPIC_STATUS] = CONTRACT_TOPIC_STATUS,
    [MQTT_LINK_TOPIC_EVENT] = CONTRACT_TOPIC_EVENT,
    [MQTT_LINK_TOPIC_STATE] = CONTRACT_TOPIC_STATE,
    [MQTT_LINK_TOPIC_CMD] = CONTRACT_TOPIC_CMD,
    [MQTT_LINK_TOPIC_CMD_ACK] = CONTRACT_TOPIC_CMD_ACK,
    [MQTT_LINK_TOPIC_CONFIG] = CONTRACT_TOPIC_CONFIG,
};

static mqtt_link_config_t s_config;
static esp_mqtt_client_handle_t s_client;
static char s_topics[MQTT_LINK_TOPIC_COUNT][CONTRACT_TOPIC_MAX_LEN];
static atomic_bool s_connected;

/* Assinaturas: escritas pela tarefa do cliente (CONNECTED/SUBSCRIBED) e pela
 * mqtt_task (mqtt_link_poll). Assinar de novo é idempotente no MQTT, então
 * uma corrida entre as duas só gera um SUBSCRIBE a mais. */
typedef enum {
    SUB_NONE = 0,
    SUB_PENDING,
    SUB_OK,
    SUB_REFUSED,
} sub_state_t;

static atomic_int s_sub_state[MQTT_LINK_TOPIC_COUNT];
static atomic_int s_sub_msg_id[MQTT_LINK_TOPIC_COUNT];
static atomic_uint_least32_t s_sub_at_ms[MQTT_LINK_TOPIC_COUNT];

static const mqtt_link_topic_t SUBSCRIBED_TOPICS[] = {
    MQTT_LINK_TOPIC_TELEMETRY_ACK,
    MQTT_LINK_TOPIC_CMD,
    MQTT_LINK_TOPIC_CONFIG,
};
#define SUBSCRIBED_COUNT (sizeof(SUBSCRIBED_TOPICS) / sizeof(SUBSCRIBED_TOPICS[0]))

/* Remontagem de mensagem que chega em pedaços (só a tarefa do cliente usa). */
static char s_rx_buffer[MQTT_LINK_RX_MAX + 1U];
static mqtt_link_topic_t s_rx_topic;
static bool s_rx_valid;

static bool is_control_topic(mqtt_link_topic_t topic)
{
    return (topic == MQTT_LINK_TOPIC_STATE) || (topic == MQTT_LINK_TOPIC_CMD) ||
           (topic == MQTT_LINK_TOPIC_CMD_ACK) || (topic == MQTT_LINK_TOPIC_CONFIG);
}

static bool topic_allowed(mqtt_link_topic_t topic)
{
    return (topic < MQTT_LINK_TOPIC_COUNT) && (s_config.control_topics || !is_control_topic(topic));
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void subscribe(mqtt_link_topic_t topic)
{
    if (!topic_allowed(topic)) {
        /* Sem assinatura o broker não entrega nada deste tópico à placa. */
        ESP_LOGI(TAG, "NÃO assinado (placa sem relés): %s", s_topics[topic]);
        return;
    }
    const int msg_id = esp_mqtt_client_subscribe(s_client, s_topics[topic], CONTRACT_QOS);
    atomic_store(&s_sub_at_ms[topic], now_ms());
    if (msg_id < 0) {
        /* Fica PENDING sem msg_id: o mqtt_link_poll tenta de novo. */
        atomic_store(&s_sub_msg_id[topic], -1);
        atomic_store(&s_sub_state[topic], SUB_PENDING);
        ESP_LOGE(TAG, "falha ao enviar SUBSCRIBE de %s", s_topics[topic]);
        return;
    }
    atomic_store(&s_sub_msg_id[topic], msg_id);
    atomic_store(&s_sub_state[topic], SUB_PENDING);
    ESP_LOGI(TAG, "SUBSCRIBE enviado (msg %d): %s", msg_id, s_topics[topic]);
}

/* SUBACK do MQTT 3.1.1: um código por filtro; 0..2 = QoS concedido, 0x80 = recusa. */
static void handle_suback(const esp_mqtt_event_handle_t event)
{
    for (size_t i = 0; i < SUBSCRIBED_COUNT; i++) {
        const mqtt_link_topic_t topic = SUBSCRIBED_TOPICS[i];
        if (atomic_load(&s_sub_msg_id[topic]) != event->msg_id) {
            continue;
        }
        const uint8_t code = ((event->data != NULL) && (event->data_len > 0))
                                 ? (uint8_t)event->data[0]
                                 : (uint8_t)SUBACK_FAILURE;
        atomic_store(&s_sub_at_ms[topic], now_ms());
        if (code >= SUBACK_FAILURE) {
            atomic_store(&s_sub_state[topic], SUB_REFUSED);
            ESP_LOGE(TAG, "SUBACK RECUSADO (0x%02x): %s", (unsigned)code, s_topics[topic]);
        } else {
            atomic_store(&s_sub_state[topic], SUB_OK);
            ESP_LOGI(TAG, "SUBACK ok (QoS %u): %s", (unsigned)code, s_topics[topic]);
        }
        return;
    }
    ESP_LOGW(TAG, "SUBACK de msg %d sem assinatura correspondente", event->msg_id);
}

static void subscribe_all(void)
{
    for (size_t i = 0; i < SUBSCRIBED_COUNT; i++) {
        subscribe(SUBSCRIBED_TOPICS[i]);
    }
}

static bool topic_from_name(const char *name, size_t len, mqtt_link_topic_t *topic)
{
    /* Só os tópicos que este firmware assina. */
    for (size_t i = 0; i < SUBSCRIBED_COUNT; i++) {
        const char *candidate = s_topics[SUBSCRIBED_TOPICS[i]];
        if (topic_allowed(SUBSCRIBED_TOPICS[i]) && (strlen(candidate) == len) &&
            (memcmp(candidate, name, len) == 0)) {
            *topic = SUBSCRIBED_TOPICS[i];
            return true;
        }
    }
    return false;
}

static void handle_data(const esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        /* Primeiro pedaço: é o único que traz o nome do tópico. */
        s_rx_valid = (event->total_data_len >= 0) &&
                     ((size_t)event->total_data_len <= MQTT_LINK_RX_MAX) &&
                     topic_from_name(event->topic, (size_t)event->topic_len, &s_rx_topic);
        if (!s_rx_valid) {
            ESP_LOGW(TAG, "mensagem descartada (tópico inesperado ou %d bytes)",
                     event->total_data_len);
        }
    }
    if (!s_rx_valid) {
        return;
    }

    const size_t end = (size_t)event->current_data_offset + (size_t)event->data_len;
    if (end > MQTT_LINK_RX_MAX) {
        s_rx_valid = false;
        return;
    }
    memcpy(&s_rx_buffer[event->current_data_offset], event->data, (size_t)event->data_len);

    if (end == (size_t)event->total_data_len) {
        s_rx_buffer[end] = '\0';
        s_rx_valid = false;
        if (s_config.on_rx != NULL) {
            s_config.on_rx(s_rx_topic, s_rx_buffer, end);
        }
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        /* clean_session = true: o broker não guarda assinaturas, então
         * assina tudo de novo a cada conexão. Nunca curingas. A config
         * retida chega logo depois do SUBACK de /config. */
        subscribe_all();
        atomic_store(&s_connected, true);
        ESP_LOGI(TAG, "conectado como %s", s_config.identity->device_id);
        if (s_config.on_conn != NULL) {
            s_config.on_conn(true);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s_connected, false);
        s_rx_valid = false;
        for (size_t i = 0; i < SUBSCRIBED_COUNT; i++) {
            atomic_store(&s_sub_state[SUBSCRIBED_TOPICS[i]], SUB_NONE);
            atomic_store(&s_sub_msg_id[SUBSCRIBED_TOPICS[i]], -1);
        }
        ESP_LOGW(TAG, "desconectado");
        if (s_config.on_conn != NULL) {
            s_config.on_conn(false);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        handle_suback(event);
        break;

    case MQTT_EVENT_DATA:
        handle_data(event);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "erro do cliente MQTT (tipo %d)", (int)event->error_handle->error_type);
        break;

    default:
        break;
    }
}

fw_err_t mqtt_link_init(const mqtt_link_config_t *config)
{
    if ((config == NULL) || (config->uri == NULL) || (config->identity == NULL) ||
        (config->identity->cert_pem == NULL) || (config->identity->key_pem == NULL) ||
        (config->identity->ca_pem == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }
    if (s_client != NULL) {
        return FW_ERR_STATE;
    }
    s_config = *config;

    for (uint32_t i = 0; i < MQTT_LINK_TOPIC_COUNT; i++) {
        const int32_t n = snprintf(s_topics[i], sizeof(s_topics[i]), "%s%s%s",
                                   CONTRACT_TOPIC_PREFIX, config->identity->device_id,
                                   TOPIC_SUFFIX[i]);
        if ((n <= 0) || ((size_t)n >= sizeof(s_topics[i]))) {
            return FW_ERR_INVALID_PARAM;
        }
    }

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = config->uri,
        /* O servidor é sempre verificado contra a CA do projeto. Não existe
         * opção para desligar isso. */
        .broker.verification.certificate = config->identity->ca_pem,
        /* O EMQX troca clientid/username pelo CN; mandamos igual para os logs baterem. */
        .credentials.client_id = config->identity->device_id,
        .credentials.authentication.certificate = config->identity->cert_pem,
        .credentials.authentication.key = config->identity->key_pem,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = CONTRACT_MQTT_KEEPALIVE_S,
        .session.disable_clean_session = false,
        .session.last_will.topic = s_topics[MQTT_LINK_TOPIC_STATUS],
        .session.last_will.msg = CONTRACT_LWT_PAYLOAD,
        .session.last_will.msg_len = (int)strlen(CONTRACT_LWT_PAYLOAD),
        .session.last_will.qos = CONTRACT_QOS,
        .session.last_will.retain = 1,
        /* A reconexão é comandada pela mqtt_task (backoff exponencial com jitter). */
        .network.disable_auto_reconnect = true,
        .network.timeout_ms = NETWORK_TIMEOUT_MS,
        .buffer.size = RX_BUFFER_BYTES,
        .buffer.out_size = TX_BUFFER_BYTES,
    };

    s_client = esp_mqtt_client_init(&mqtt_config);
    if (s_client == NULL) {
        return FW_ERR_NO_MEM;
    }
    if (esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, event_handler, NULL) != ESP_OK) {
        return FW_ERR_STATE;
    }
    return FW_OK;
}

fw_err_t mqtt_link_start(void)
{
    if (s_client == NULL) {
        return FW_ERR_STATE;
    }
    return (esp_mqtt_client_start(s_client) == ESP_OK) ? FW_OK : FW_ERR_IO;
}

fw_err_t mqtt_link_reconnect(void)
{
    if (s_client == NULL) {
        return FW_ERR_STATE;
    }
    return (esp_mqtt_client_reconnect(s_client) == ESP_OK) ? FW_OK : FW_ERR_IO;
}

void mqtt_link_poll(void)
{
    if ((s_client == NULL) || !mqtt_link_is_connected()) {
        return;
    }
    const uint32_t now = now_ms();
    for (size_t i = 0; i < SUBSCRIBED_COUNT; i++) {
        const mqtt_link_topic_t topic = SUBSCRIBED_TOPICS[i];
        const int state = atomic_load(&s_sub_state[topic]);
        const uint32_t age = now - (uint32_t)atomic_load(&s_sub_at_ms[topic]);
        if (((state == SUB_PENDING) && (age >= SUBACK_TIMEOUT_MS)) ||
            ((state == SUB_REFUSED) && (age >= SUB_RETRY_MS))) {
            ESP_LOGW(TAG, "%s sem SUBACK válido há %u ms; assinando de novo", s_topics[topic],
                     (unsigned)age);
            subscribe(topic);
        }
    }
}

bool mqtt_link_is_connected(void)
{
    return atomic_load(&s_connected);
}

fw_err_t mqtt_link_publish(mqtt_link_topic_t topic, const char *payload, size_t len, bool retain)
{
    if ((payload == NULL) || (topic >= MQTT_LINK_TOPIC_COUNT)) {
        return FW_ERR_INVALID_PARAM;
    }
    /* Última barreira: tópico de controle com o controle desligado derrubaria
     * a conexão, então nem chega ao cliente. */
    if ((s_client == NULL) || !topic_allowed(topic) || !mqtt_link_is_connected()) {
        return FW_ERR_STATE;
    }
    const int32_t msg_id = esp_mqtt_client_enqueue(s_client, s_topics[topic], payload, (int)len,
                                                   CONTRACT_QOS, retain ? 1 : 0, true);
    return (msg_id >= 0) ? FW_OK : FW_ERR_FULL;
}
