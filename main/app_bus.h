#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_err.h"
#include "mqtt_link.h"

/*
 * Barramento interno: filas de saída para o broker e valores compartilhados
 * entre tarefas (sempre por função, nunca por variável global).
 */

#define APP_BUS_PAYLOAD_MAX    (1024U)
#define APP_BUS_EVENT_TYPE_MAX (32U)
#define APP_BUS_EVENT_DATA_MAX (192U)

typedef struct {
    mqtt_link_topic_t topic;
    bool retain;
    uint16_t len;
    char payload[APP_BUS_PAYLOAD_MAX];
} app_bus_pub_t;

typedef struct {
    char type[APP_BUS_EVENT_TYPE_MAX];
    char data[APP_BUS_EVENT_DATA_MAX]; /* objeto JSON, ou "" */
    uint64_t at_ms;                    /* instante monotônico do fato */
} app_bus_event_t;

/** @brief Cria as filas e gera o boot_id. Chamar antes de iniciar o Wi-Fi. */
fw_err_t app_bus_init(void);

/** @brief 8 bytes aleatórios em hex, novos a cada boot. */
const char *app_bus_boot_id(void);

/** @brief Versão do firmware (version.txt). */
const char *app_bus_fw_version(void);

/**
 * @brief Enfileira uma publicação para a mqtt_task.
 *
 * @return false se a fila está cheia ou o payload não cabe
 */
bool app_bus_publish(mqtt_link_topic_t topic, bool retain, const char *payload, size_t len);

/**
 * @brief Registra um DeviceEvent. Fica guardado enquanto não há conexão.
 *
 * @param type     Um dos CONTRACT_EV_*
 * @param data_fmt Formato printf de um OBJETO JSON, ou NULL para sem `data`
 */
void app_bus_post_event(const char *type, const char *data_fmt, ...)
    __attribute__((format(printf, 2, 3)));

/** @brief (mqtt_task) Próxima publicação, esperando até `timeout_ms`. */
bool app_bus_take_publish(app_bus_pub_t *out, uint32_t timeout_ms);

/** @brief (mqtt_task) Próximo evento pendente, sem esperar. */
bool app_bus_take_event(app_bus_event_t *out);

/** @brief Intervalo de telemetria (Kconfig). */
uint32_t app_bus_telemetry_interval_s(void);

/** @brief Fail-safe dos relés em vigor (config; 60 s até chegar uma). */
void app_bus_set_failsafe_s(uint32_t failsafe_s);
uint32_t app_bus_failsafe_s(void);

/** @brief Contadores do journal, publicados em status e telemetria. */
void app_bus_set_journal_stats(uint32_t pending, uint32_t dropped);
uint32_t app_bus_journal_pending(void);
uint32_t app_bus_journal_dropped(void);
