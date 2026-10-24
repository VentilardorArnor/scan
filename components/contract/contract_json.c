#include "contract_json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "fw_time.h"

#define SECONDS_PER_DAY    (86400LL)
#define DAYS_PER_ERA       (146097LL)
#define DAYS_TO_UNIX_EPOCH (719468LL)
#define YEARS_PER_ERA      (400LL)
#define TS_MIN_LEN         (20U)
#define SCALE_COORD        (1e7)  /* ~1 cm: mais que a precisão do receptor */
#define SCALE_1_DECIMAL    (10.0)
#define SCALE_2_DECIMALS   (100.0)

/* ------------------------------------------------------------------ */
/* Tempo                                                               */
/* ------------------------------------------------------------------ */

fw_err_t contract_time_format(int64_t epoch, char out[CONTRACT_TS_STR_LEN])
{
    if ((out == NULL) || (epoch < 0)) {
        return FW_ERR_INVALID_PARAM;
    }

    const int64_t days = epoch / SECONDS_PER_DAY;
    const uint32_t rem = (uint32_t)(epoch % SECONDS_PER_DAY);

    const int64_t z = days + DAYS_TO_UNIX_EPOCH;
    const int64_t era = z / DAYS_PER_ERA;
    const uint32_t doe = (uint32_t)(z - (era * DAYS_PER_ERA));
    const uint32_t yoe = (doe - (doe / 1460U) + (doe / 36524U) - (doe / 146096U)) / 365U;
    const uint32_t doy = doe - ((365U * yoe) + (yoe / 4U) - (yoe / 100U));
    const uint32_t mp = ((5U * doy) + 2U) / 153U;
    const uint32_t day = doy - (((153U * mp) + 2U) / 5U) + 1U;
    const uint32_t month = (mp < 10U) ? (mp + 3U) : (mp - 9U);
    const int64_t year = (int64_t)yoe + (era * YEARS_PER_ERA) + ((month <= 2U) ? 1 : 0);

    const int32_t n = snprintf(out, CONTRACT_TS_STR_LEN, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                               (unsigned)year, (unsigned)month, (unsigned)day,
                               (unsigned)(rem / 3600U), (unsigned)((rem % 3600U) / 60U),
                               (unsigned)(rem % 60U));
    return ((n > 0) && ((size_t)n < CONTRACT_TS_STR_LEN)) ? FW_OK : FW_ERR_INVALID_PARAM;
}

static bool parse_digits(const char *s, size_t count, uint32_t *value)
{
    uint32_t v = 0;
    for (size_t i = 0; i < count; i++) {
        if ((s[i] < '0') || (s[i] > '9')) {
            return false;
        }
        v = (v * 10U) + (uint32_t)(s[i] - '0');
    }
    *value = v;
    return true;
}

fw_err_t contract_time_parse(const char *str, int64_t *epoch)
{
    if ((str == NULL) || (epoch == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }
    if (strlen(str) < TS_MIN_LEN) {
        return FW_ERR_INVALID_DATA;
    }

    uint32_t year = 0;
    uint32_t month = 0;
    uint32_t day = 0;
    uint32_t hour = 0;
    uint32_t minute = 0;
    uint32_t second = 0;
    const bool shape_ok = parse_digits(&str[0], 4, &year) && (str[4] == '-') &&
                          parse_digits(&str[5], 2, &month) && (str[7] == '-') &&
                          parse_digits(&str[8], 2, &day) && (str[10] == 'T') &&
                          parse_digits(&str[11], 2, &hour) && (str[13] == ':') &&
                          parse_digits(&str[14], 2, &minute) && (str[16] == ':') &&
                          parse_digits(&str[17], 2, &second);
    if (!shape_ok) {
        return FW_ERR_INVALID_DATA;
    }

    /* Fração de segundo opcional. Fuso obrigatório: 'Z' ou ±HH:MM (RFC 3339,
     * `format: date-time` do schema). A placa sempre escreve com 'Z'. */
    const char *p = &str[19];
    if (*p == '.') {
        p++;
        while ((*p >= '0') && (*p <= '9')) {
            p++;
        }
    }
    int64_t offset_s = 0;
    if ((p[0] == 'Z') && (p[1] == '\0')) {
        /* UTC */
    } else if ((p[0] == '+') || (p[0] == '-')) {
        uint32_t off_h = 0;
        uint32_t off_m = 0;
        if (!parse_digits(&p[1], 2, &off_h) || (p[3] != ':') || !parse_digits(&p[4], 2, &off_m) ||
            (p[6] != '\0') || (off_h > 23U) || (off_m > 59U)) {
            return FW_ERR_INVALID_DATA;
        }
        offset_s = (int64_t)((off_h * 3600U) + (off_m * 60U));
        if (p[0] == '-') {
            offset_s = -offset_s;
        }
    } else {
        return FW_ERR_INVALID_DATA;
    }
    if ((year < 1970U) || (month < 1U) || (month > 12U) || (day < 1U) || (day > 31U) ||
        (hour > 23U) || (minute > 59U) || (second > 60U)) {
        return FW_ERR_INVALID_DATA;
    }

    /* Hora local = UTC + deslocamento. */
    *epoch = fw_time_civil_to_epoch((int64_t)year, month, day, hour, minute, second) - offset_s;
    return FW_OK;
}

/* ------------------------------------------------------------------ */
/* Auxiliares de cJSON                                                 */
/* ------------------------------------------------------------------ */

static double round_to(double value, double scale)
{
    return round(value * scale) / scale;
}

static void add_num(cJSON *obj, const char *key, double value, bool *ok)
{
    if (cJSON_AddNumberToObject(obj, key, value) == NULL) {
        *ok = false;
    }
}

static void add_str(cJSON *obj, const char *key, const char *value, bool *ok)
{
    if (cJSON_AddStringToObject(obj, key, value) == NULL) {
        *ok = false;
    }
}

static void add_bool(cJSON *obj, const char *key, bool value, bool *ok)
{
    if (cJSON_AddBoolToObject(obj, key, value) == NULL) {
        *ok = false;
    }
}

static void add_ts(cJSON *obj, int64_t ts, bool *ok)
{
    char buf[CONTRACT_TS_STR_LEN];
    if (contract_time_format(ts, buf) != FW_OK) {
        *ok = false;
        return;
    }
    add_str(obj, CONTRACT_K_TS, buf, ok);
}

/* Imprime, libera a árvore e traduz o resultado. */
static fw_err_t finish(cJSON *root, bool ok, char *out, size_t cap)
{
    fw_err_t err = FW_ERR_NO_MEM;
    if (ok && (cJSON_PrintPreallocated(root, out, (int)cap, false) != 0)) {
        err = FW_OK;
    }
    cJSON_Delete(root);
    return err;
}

static bool get_u32(const cJSON *obj, const char *key, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double v = item->valuedouble;
    if ((v < 0.0) || (v > (double)UINT32_MAX) || (v != floor(v))) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static const char *get_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* ------------------------------------------------------------------ */
/* Telemetria                                                          */
/* ------------------------------------------------------------------ */

static const char *time_source_str(contract_time_source_t src)
{
    switch (src) {
    case CONTRACT_TIME_NTP:
        return CONTRACT_TIME_SOURCE_NTP;
    case CONTRACT_TIME_GPS:
        return CONTRACT_TIME_SOURCE_GPS;
    case CONTRACT_TIME_RTC:
        return CONTRACT_TIME_SOURCE_RTC;
    case CONTRACT_TIME_NONE:
    default:
        return CONTRACT_TIME_SOURCE_NONE;
    }
}

fw_err_t contract_build_telemetry(const contract_telemetry_t *t, char *out, size_t cap)
{
    if ((t == NULL) || (out == NULL) || (t->boot_id == NULL) || (t->fw == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return FW_ERR_NO_MEM;
    }
    bool ok = true;

    add_num(root, CONTRACT_K_V, CONTRACT_V_TELEMETRY, &ok);
    add_ts(root, t->ts, &ok);
    add_str(root, CONTRACT_K_FW, t->fw, &ok);
    add_str(root, CONTRACT_K_BOOT_ID, t->boot_id, &ok);
    add_num(root, CONTRACT_K_SEQ, (double)t->seq, &ok);
    add_str(root, CONTRACT_K_TIME_SOURCE, time_source_str(t->time_source), &ok);

    /* Sem sensores agronômicos nesta placa, mas o array é obrigatório. */
    if (cJSON_AddArrayToObject(root, CONTRACT_K_READINGS) == NULL) {
        ok = false;
    }

    cJSON *sys = cJSON_AddObjectToObject(root, CONTRACT_K_SYS);
    if (sys == NULL) {
        ok = false;
    } else {
        if (t->has_rssi) {
            add_num(sys, CONTRACT_K_SYS_RSSI, (double)t->rssi, &ok);
        }
        add_num(sys, CONTRACT_K_SYS_HEAP_FREE, (double)t->heap_free, &ok);
        add_num(sys, CONTRACT_K_JOURNAL_PENDING, (double)t->journal_pending, &ok);
    }

    if (t->has_gps) {
        cJSON *gps = cJSON_AddObjectToObject(root, CONTRACT_K_GPS);
        if (gps == NULL) {
            ok = false;
        } else {
            add_num(gps, CONTRACT_K_GPS_LAT, round_to(t->gps.lat, SCALE_COORD), &ok);
            add_num(gps, CONTRACT_K_GPS_LON, round_to(t->gps.lon, SCALE_COORD), &ok);
            if (t->gps.has_hdop) {
                add_num(gps, CONTRACT_K_GPS_HDOP, round_to(t->gps.hdop, SCALE_2_DECIMALS), &ok);
            }
            if (t->gps.has_sats) {
                add_num(gps, CONTRACT_K_GPS_SATS, (double)t->gps.sats, &ok);
            }
            if (t->gps.has_alt) {
                add_num(gps, CONTRACT_K_GPS_ALT_M, round_to(t->gps.alt_m, SCALE_1_DECIMAL), &ok);
            }
            if (t->gps.has_speed) {
                add_num(gps, CONTRACT_K_GPS_SPEED_KMH, round_to(t->gps.speed_kmh, SCALE_1_DECIMAL),
                        &ok);
            }
            if (t->gps.has_course) {
                add_num(gps, CONTRACT_K_GPS_COURSE_DEG,
                        round_to(t->gps.course_deg, SCALE_1_DECIMAL), &ok);
            }
        }
    }

    return finish(root, ok, out, cap);
}

fw_err_t contract_parse_telemetry_ack(const char *json, size_t len, contract_telemetry_ack_t *ack)
{
    if ((json == NULL) || (ack == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return FW_ERR_INVALID_DATA;
    }

    fw_err_t err = FW_ERR_INVALID_DATA;
    memset(ack, 0, sizeof(*ack));

    const char *boot_id = get_str(root, CONTRACT_K_BOOT_ID);
    const char *status = get_str(root, CONTRACT_K_STATUS);
    const char *reason = get_str(root, CONTRACT_K_REASON);
    const bool fields_ok = (boot_id != NULL) && (status != NULL) &&
                           (strlen(boot_id) <= CONTRACT_BOOT_ID_MAX_LEN) &&
                           get_u32(root, CONTRACT_K_SEQ, &ack->seq);

    if (fields_ok) {
        err = FW_OK;
        (void)snprintf(ack->boot_id, sizeof(ack->boot_id), "%s", boot_id);

        if (strcmp(status, CONTRACT_ACK_STORED) == 0) {
            ack->status = CONTRACT_ACK_ST_STORED;
        } else if (strcmp(status, CONTRACT_ACK_DUPLICATE) == 0) {
            ack->status = CONTRACT_ACK_ST_DUPLICATE;
        } else if (strcmp(status, CONTRACT_ACK_REJECTED) == 0) {
            ack->status = CONTRACT_ACK_ST_REJECTED;
            if (reason == NULL) {
                ack->reason = CONTRACT_ACK_RSN_UNKNOWN;
            } else if (strcmp(reason, CONTRACT_ACK_REASON_INVALID) == 0) {
                ack->reason = CONTRACT_ACK_RSN_INVALID;
            } else if (strcmp(reason, CONTRACT_ACK_REASON_UNCLAIMED) == 0) {
                ack->reason = CONTRACT_ACK_RSN_UNCLAIMED;
            } else if (strcmp(reason, CONTRACT_ACK_REASON_REVOKED) == 0) {
                ack->reason = CONTRACT_ACK_RSN_REVOKED;
            } else if (strcmp(reason, CONTRACT_ACK_REASON_CLOCK_SKEW) == 0) {
                ack->reason = CONTRACT_ACK_RSN_CLOCK_SKEW;
            } else {
                ack->reason = CONTRACT_ACK_RSN_UNKNOWN;
            }
        } else {
            err = FW_ERR_INVALID_DATA; /* status desconhecido: não libera nada */
        }
    }

    cJSON_Delete(root);
    return err;
}

/* ------------------------------------------------------------------ */
/* Status e eventos                                                    */
/* ------------------------------------------------------------------ */

fw_err_t contract_build_status(const contract_status_t *s, char *out, size_t cap)
{
    if ((s == NULL) || (out == NULL) || (s->boot_id == NULL) || (s->fw == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return FW_ERR_NO_MEM;
    }
    bool ok = true;

    add_num(root, CONTRACT_K_V, CONTRACT_V_STATUS, &ok);
    add_bool(root, CONTRACT_K_ONLINE, true, &ok);
    add_ts(root, s->ts, &ok);
    add_str(root, CONTRACT_K_FW, s->fw, &ok);
    add_str(root, CONTRACT_K_BOOT_ID, s->boot_id, &ok);
    add_num(root, CONTRACT_K_UPTIME_S, (double)s->uptime_s, &ok);
    if (s->has_rssi) {
        add_num(root, CONTRACT_K_RSSI, (double)s->rssi, &ok);
    }
    add_num(root, CONTRACT_K_JOURNAL_PENDING, (double)s->journal_pending, &ok);
    add_num(root, CONTRACT_K_JOURNAL_DROPPED, (double)s->journal_dropped, &ok);

    return finish(root, ok, out, cap);
}

fw_err_t contract_build_event(int64_t ts, const char *type, const char *data_json, char *out,
                              size_t cap)
{
    if ((type == NULL) || (out == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return FW_ERR_NO_MEM;
    }
    bool ok = true;

    add_num(root, CONTRACT_K_V, CONTRACT_V_EVENT, &ok);
    add_ts(root, ts, &ok);
    add_str(root, CONTRACT_K_TYPE, type, &ok);

    if ((data_json != NULL) && (data_json[0] != '\0')) {
        /* Reanalisa em vez de inserir cru: garante que `data` é JSON válido. */
        cJSON *data = cJSON_Parse(data_json);
        if (!cJSON_IsObject(data) || !cJSON_AddItemToObject(root, CONTRACT_K_DATA, data)) {
            cJSON_Delete(data);
            ok = false;
        }
    }

    return finish(root, ok, out, cap);
}

/* ------------------------------------------------------------------ */
/* Controle: state, cmd, cmd/ack                                       */
/* ------------------------------------------------------------------ */

static void add_bool_array(cJSON *root, const char *key, uint8_t count, uint16_t mask, bool *ok)
{
    cJSON *arr = cJSON_AddArrayToObject(root, key);
    if (arr == NULL) {
        *ok = false;
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateBool((mask & (uint16_t)(1U << i)) != 0U);
        if ((item == NULL) || !cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
            *ok = false;
            return;
        }
    }
}

fw_err_t contract_build_state(const contract_state_t *s, char *out, size_t cap)
{
    if ((s == NULL) || (out == NULL) || (s->relay_count > CONTRACT_CHANNEL_MAX) ||
        (s->input_count > CONTRACT_INPUT_MAX)) {
        return FW_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return FW_ERR_NO_MEM;
    }
    bool ok = true;

    add_num(root, CONTRACT_K_V, CONTRACT_V_STATE, &ok);
    add_ts(root, s->ts, &ok);
    add_num(root, CONTRACT_K_CONFIG_VERSION, (double)s->config_version, &ok);
    add_bool(root, CONTRACT_K_ESTOP, s->estop, &ok);
    add_bool_array(root, CONTRACT_K_RELAYS, s->relay_count, s->relays_mask, &ok);
    add_bool_array(root, CONTRACT_K_INPUTS, s->input_count, s->inputs_mask, &ok);

    return finish(root, ok, out, cap);
}

static contract_cmd_type_t cmd_type_from_str(const char *type)
{
    if (type == NULL) {
        return CONTRACT_CMD_T_UNKNOWN;
    }
    if (strcmp(type, CONTRACT_CMD_RELAY_PULSE) == 0) {
        return CONTRACT_CMD_T_RELAY_PULSE;
    }
    if (strcmp(type, CONTRACT_CMD_ESTOP) == 0) {
        return CONTRACT_CMD_T_ESTOP;
    }
    return CONTRACT_CMD_T_UNKNOWN;
}

fw_err_t contract_parse_command(const char *json, size_t len, contract_command_t *cmd)
{
    if ((json == NULL) || (cmd == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return FW_ERR_INVALID_DATA;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = CONTRACT_CMD_T_UNKNOWN;

    const char *id = get_str(root, CONTRACT_K_ID);
    if ((id == NULL) || (id[0] == '\0') || (strlen(id) > CONTRACT_COMMAND_ID_MAX_LEN)) {
        cJSON_Delete(root);
        return FW_ERR_INVALID_DATA; /* sem id não há como responder */
    }
    (void)snprintf(cmd->id, sizeof(cmd->id), "%s", id);

    uint32_t version = CONTRACT_V_CMD;
    const bool has_v = (cJSON_GetObjectItemCaseSensitive(root, CONTRACT_K_V) != NULL);
    cmd->version_ok = !has_v || (get_u32(root, CONTRACT_K_V, &version) && (version == CONTRACT_V_CMD));
    cmd->type = cmd_type_from_str(get_str(root, CONTRACT_K_TYPE));

    const char *expires_at = get_str(root, CONTRACT_K_EXPIRES_AT);
    const char *issued_at = get_str(root, CONTRACT_K_ISSUED_AT);
    int64_t issued = 0;
    cmd->fields_ok = (expires_at != NULL) && (issued_at != NULL) &&
                     (contract_time_parse(expires_at, &cmd->expires_at) == FW_OK) &&
                     (contract_time_parse(issued_at, &issued) == FW_OK) &&
                     get_u32(root, CONTRACT_K_TTL_MS, &cmd->ttl_ms) &&
                     (cmd->ttl_ms <= CONTRACT_TTL_MAX_MS);

    if (cmd->type == CONTRACT_CMD_T_RELAY_PULSE) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, CONTRACT_K_PARAMS);
        uint32_t channel = 0;
        cmd->params_ok = cJSON_IsObject(params) && get_u32(params, CONTRACT_K_CHANNEL, &channel) &&
                         (channel < CONTRACT_CHANNEL_MAX) &&
                         get_u32(params, CONTRACT_K_PULSE_MS, &cmd->pulse_ms) &&
                         (cmd->pulse_ms >= CONTRACT_PULSE_MIN_MS) &&
                         (cmd->pulse_ms <= CONTRACT_PULSE_MAX_MS);
        cmd->channel = (uint8_t)channel;
    } else {
        cmd->params_ok = (cmd->type == CONTRACT_CMD_T_ESTOP); /* estop não tem params */
    }

    cJSON_Delete(root);
    return FW_OK;
}

static const char *cmd_status_str(contract_cmd_status_t status)
{
    switch (status) {
    case CONTRACT_CMD_STATUS_RECEIVED:
        return CONTRACT_CMD_ST_RECEIVED;
    case CONTRACT_CMD_STATUS_COMPLETED:
        return CONTRACT_CMD_ST_COMPLETED;
    case CONTRACT_CMD_STATUS_REJECTED:
        return CONTRACT_CMD_ST_REJECTED;
    case CONTRACT_CMD_STATUS_FAILED:
    default:
        return CONTRACT_CMD_ST_FAILED;
    }
}

fw_err_t contract_build_cmd_ack(const contract_cmd_ack_t *ack, char *out, size_t cap)
{
    if ((ack == NULL) || (out == NULL) || (ack->id == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return FW_ERR_NO_MEM;
    }
    bool ok = true;

    add_num(root, CONTRACT_K_V, CONTRACT_V_CMD_ACK, &ok);
    add_str(root, CONTRACT_K_ID, ack->id, &ok);
    add_str(root, CONTRACT_K_STATUS, cmd_status_str(ack->status), &ok);
    if ((ack->reason != NULL) && (ack->reason[0] != '\0')) {
        add_str(root, CONTRACT_K_REASON, ack->reason, &ok);
    }
    if (ack->has_ts) {
        add_ts(root, ack->ts, &ok);
    }

    return finish(root, ok, out, cap);
}
