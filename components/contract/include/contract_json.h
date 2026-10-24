#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "contract.h"
#include "fw_err.h"

#define CONTRACT_TS_STR_LEN (21U) /* "2026-10-13T12:00:00Z" + NUL */

typedef enum {
    CONTRACT_TIME_NONE = 0,
    CONTRACT_TIME_NTP,
    CONTRACT_TIME_GPS,
    CONTRACT_TIME_RTC,
} contract_time_source_t;

typedef struct {
    double lat;
    double lon;
    bool has_hdop;
    float hdop;
    bool has_sats;
    uint8_t sats;
    bool has_alt;
    float alt_m;
    bool has_speed;
    float speed_kmh;
    bool has_course;
    float course_deg;
} contract_gps_t;

typedef struct {
    int64_t ts;
    const char *fw;
    const char *boot_id;
    uint32_t seq;
    contract_time_source_t time_source;
    bool has_rssi;
    int32_t rssi;
    uint32_t heap_free;
    uint32_t journal_pending;
    bool has_gps; /* false = objeto gps omitido (nunca 0,0) */
    contract_gps_t gps;
} contract_telemetry_t;

typedef enum {
    CONTRACT_ACK_ST_STORED = 0,
    CONTRACT_ACK_ST_DUPLICATE,
    CONTRACT_ACK_ST_REJECTED,
} contract_ack_status_t;

typedef enum {
    CONTRACT_ACK_RSN_NONE = 0,
    CONTRACT_ACK_RSN_INVALID,
    CONTRACT_ACK_RSN_UNCLAIMED,
    CONTRACT_ACK_RSN_REVOKED,
    CONTRACT_ACK_RSN_CLOCK_SKEW,
    CONTRACT_ACK_RSN_UNKNOWN,
} contract_ack_reason_t;

typedef struct {
    char boot_id[CONTRACT_BOOT_ID_MAX_LEN + 1U];
    uint32_t seq;
    contract_ack_status_t status;
    contract_ack_reason_t reason;
} contract_telemetry_ack_t;

typedef struct {
    int64_t ts;
    const char *fw;
    const char *boot_id;
    uint32_t uptime_s;
    bool has_rssi;
    int32_t rssi;
    uint32_t journal_pending;
    uint32_t journal_dropped;
} contract_status_t;

/* State (state.schema.json): relés e entradas como a placa os vê. */
typedef struct {
    int64_t ts;
    uint32_t config_version; /* 0 = ainda sem config */
    bool estop;
    uint8_t relay_count;   /* relés instalados (posição = canal) */
    uint16_t relays_mask;  /* bit n = relé do canal n fechado */
    uint8_t input_count;
    uint16_t inputs_mask;
} contract_state_t;

typedef enum {
    CONTRACT_CMD_T_RELAY_PULSE = 0,
    CONTRACT_CMD_T_ESTOP,
    CONTRACT_CMD_T_UNKNOWN,
} contract_cmd_type_t;

/* CommandEnvelope (cmd.schema.json) */
typedef struct {
    char id[CONTRACT_COMMAND_ID_MAX_LEN + 1U];
    bool version_ok;  /* `v` ausente ou igual a 1 */
    contract_cmd_type_t type;
    bool fields_ok;   /* issued_at, expires_at e ttl_ms presentes e válidos */
    int64_t expires_at;
    uint32_t ttl_ms;
    bool params_ok;   /* relay.pulse: channel 0..15 e pulse_ms 100..5000 */
    uint8_t channel;
    uint32_t pulse_ms;
} contract_command_t;

typedef enum {
    CONTRACT_CMD_STATUS_RECEIVED = 0,
    CONTRACT_CMD_STATUS_COMPLETED,
    CONTRACT_CMD_STATUS_FAILED,
    CONTRACT_CMD_STATUS_REJECTED,
} contract_cmd_status_t;

/* CommandAck (cmd-ack.schema.json) */
typedef struct {
    const char *id;
    contract_cmd_status_t status;
    bool has_ts;
    int64_t ts;
    const char *reason; /* NULL ou "" = omitido */
} contract_cmd_ack_t;

/**
 * @brief Formata segundos desde a época Unix como ISO 8601 UTC ("...Z").
 *
 * @param epoch Segundos desde 1970-01-01T00:00:00Z
 * @param out   Buffer de CONTRACT_TS_STR_LEN bytes
 *
 * @return FW_OK, ou FW_ERR_INVALID_PARAM
 */
fw_err_t contract_time_format(int64_t epoch, char out[CONTRACT_TS_STR_LEN]);

/**
 * @brief Converte "YYYY-MM-DDTHH:MM:SS[.fff]Z" em segundos desde a época.
 *
 * @return FW_OK, FW_ERR_INVALID_PARAM ou FW_ERR_INVALID_DATA
 */
fw_err_t contract_time_parse(const char *str, int64_t *epoch);

/**
 * @brief Serializa um TelemetryEnvelope (§4.1).
 *
 * @return FW_OK; FW_ERR_NO_MEM se não coube em `cap`
 */
fw_err_t contract_build_telemetry(const contract_telemetry_t *t, char *out, size_t cap);

/** @brief Lê um TelemetryAck (§4.2). Campos desconhecidos são ignorados. */
fw_err_t contract_parse_telemetry_ack(const char *json, size_t len, contract_telemetry_ack_t *ack);

/** @brief Serializa um DeviceStatus com online=true (§4.3). */
fw_err_t contract_build_status(const contract_status_t *s, char *out, size_t cap);

/**
 * @brief Serializa um DeviceEvent (§4.5).
 *
 * @param data_json Objeto JSON já serializado para `data`, ou NULL para omitir
 */
fw_err_t contract_build_event(int64_t ts, const char *type, const char *data_json, char *out,
                              size_t cap);

/** @brief Serializa o State (não retido; ao mudar e a cada 10 s). */
fw_err_t contract_build_state(const contract_state_t *s, char *out, size_t cap);

/**
 * @brief Lê um CommandEnvelope.
 *
 * @return FW_OK se há JSON válido com `id` utilizável (o resto é avaliado
 *         depois, para a recusa sair com ACK); FW_ERR_INVALID_DATA se não dá
 *         nem para responder.
 */
fw_err_t contract_parse_command(const char *json, size_t len, contract_command_t *cmd);

/** @brief Serializa um CommandAck. */
fw_err_t contract_build_cmd_ack(const contract_cmd_ack_t *ack, char *out, size_t cap);
