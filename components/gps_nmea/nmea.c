#include "nmea.h"

#include <stdlib.h>
#include <string.h>

#define NMEA_FIELD_MAX        (24U)
#define NMEA_HEADER_LEN       (6U)  /* "$GPGGA" */
#define NMEA_CHECKSUM_LEN     (2U)
#define KNOTS_TO_KMH          (1.852f)
#define MINUTES_PER_DEGREE    (60.0)
#define NMEA_YEAR_BASE        (2000U)
#define GSA_FIX_NONE          (1)
#define GSA_IDX_FIX           (2U)
#define GSA_IDX_HDOP          (16U)

/* Índices de campo (0 = cabeçalho "GPGGA") */
#define GGA_IDX_TIME    (1U)
#define GGA_IDX_LAT     (2U)
#define GGA_IDX_NS      (3U)
#define GGA_IDX_LON     (4U)
#define GGA_IDX_EW      (5U)
#define GGA_IDX_QUALITY (6U)
#define GGA_IDX_SATS    (7U)
#define GGA_IDX_HDOP    (8U)
#define GGA_IDX_ALT     (9U)
#define GGA_MIN_FIELDS  (10U)

#define RMC_IDX_TIME    (1U)
#define RMC_IDX_STATUS  (2U)
#define RMC_IDX_LAT     (3U)
#define RMC_IDX_NS      (4U)
#define RMC_IDX_LON     (5U)
#define RMC_IDX_EW      (6U)
#define RMC_IDX_SPEED   (7U)
#define RMC_IDX_COURSE  (8U)
#define RMC_IDX_DATE    (9U)
#define RMC_MIN_FIELDS  (10U)

#define GSA_MIN_FIELDS  (17U)
#define GSV_IDX_IN_VIEW (3U)
#define GSV_MIN_FIELDS  (4U)

void nmea_framer_init(nmea_framer_t *framer)
{
    if (framer != NULL) {
        memset(framer, 0, sizeof(*framer));
    }
}

bool nmea_framer_feed(nmea_framer_t *framer, uint8_t byte)
{
    if (framer == NULL) {
        return false;
    }

    if (byte == (uint8_t)'$') {
        /* '$' sempre reinicia: descarta lixo ou sentença truncada. */
        framer->len = 0;
        framer->in_sentence = true;
    }
    if (!framer->in_sentence) {
        return false;
    }

    if ((byte == (uint8_t)'\r') || (byte == (uint8_t)'\n')) {
        framer->in_sentence = false;
        framer->line[framer->len] = '\0';
        return framer->len > 0U;
    }

    if (framer->len >= (NMEA_LINE_MAX - 1U)) {
        framer->in_sentence = false; /* longa demais: descarta */
        framer->len = 0;
        return false;
    }

    framer->line[framer->len] = (char)byte;
    framer->len++;
    return false;
}

static bool hex_nibble(char c, uint8_t *value)
{
    if ((c >= '0') && (c <= '9')) {
        *value = (uint8_t)(c - '0');
    } else if ((c >= 'A') && (c <= 'F')) {
        *value = (uint8_t)(c - 'A' + 10);
    } else if ((c >= 'a') && (c <= 'f')) {
        *value = (uint8_t)(c - 'a' + 10);
    } else {
        return false;
    }
    return true;
}

/* Devolve o comprimento do corpo (entre '$' e '*') se o checksum confere. */
static bool checksum_ok(const char *line, size_t *body_len)
{
    const char *star = strchr(line, '*');
    if ((line[0] != '$') || (star == NULL) || (strlen(star) != (NMEA_CHECKSUM_LEN + 1U))) {
        return false;
    }

    uint8_t expected_hi = 0;
    uint8_t expected_lo = 0;
    if (!hex_nibble(star[1], &expected_hi) || !hex_nibble(star[2], &expected_lo)) {
        return false;
    }

    uint8_t sum = 0;
    for (const char *p = &line[1]; p < star; p++) {
        sum ^= (uint8_t)*p;
    }

    *body_len = (size_t)(star - &line[1]);
    return sum == (uint8_t)((expected_hi << 4) | expected_lo);
}

/* Copia o campo `index` (separado por vírgula) para `out`. Campo vazio é
 * válido e devolve string vazia; strtok não serve porque pula campos vazios. */
static bool get_field(const char *body, size_t body_len, uint32_t index, char out[NMEA_FIELD_MAX])
{
    uint32_t current = 0;
    size_t start = 0;

    for (size_t i = 0; i <= body_len; i++) {
        if ((i == body_len) || (body[i] == ',')) {
            if (current == index) {
                const size_t field_len = i - start;
                if (field_len >= NMEA_FIELD_MAX) {
                    return false;
                }
                memcpy(out, &body[start], field_len);
                out[field_len] = '\0';
                return true;
            }
            current++;
            start = i + 1U;
        }
    }
    return false;
}

static uint32_t count_fields(const char *body, size_t body_len)
{
    uint32_t count = 1;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == ',') {
            count++;
        }
    }
    return count;
}

static bool parse_float(const char *field, float *value)
{
    if (field[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const float v = strtof(field, &end);
    if ((end == field) || (*end != '\0')) {
        return false;
    }
    *value = v;
    return true;
}

/* "ddmm.mmmm" / "dddmm.mmmm" + hemisfério -> graus decimais com sinal. */
static bool parse_coord(const char *field, const char *hemi, double max_deg, double *value)
{
    if ((field[0] == '\0') || (hemi[0] == '\0') || (hemi[1] != '\0')) {
        return false;
    }

    char *end = NULL;
    const double raw = strtod(field, &end);
    if ((end == field) || (*end != '\0') || (raw < 0.0)) {
        return false;
    }

    const double degrees = (double)((int32_t)(raw / 100.0));
    const double minutes = raw - (degrees * 100.0);
    if (minutes >= MINUTES_PER_DEGREE) {
        return false;
    }

    double result = degrees + (minutes / MINUTES_PER_DEGREE);
    if (result > max_deg) {
        return false;
    }

    if ((hemi[0] == 'S') || (hemi[0] == 'W')) {
        result = -result;
    } else if ((hemi[0] != 'N') && (hemi[0] != 'E')) {
        return false;
    } else {
        /* N ou E: mantém positivo */
    }

    *value = result;
    return true;
}

static bool two_digits(const char *s, uint8_t *value)
{
    if ((s[0] < '0') || (s[0] > '9') || (s[1] < '0') || (s[1] > '9')) {
        return false;
    }
    *value = (uint8_t)(((s[0] - '0') * 10) + (s[1] - '0'));
    return true;
}

static void parse_time(const char *field, nmea_sentence_t *out)
{
    if (strlen(field) < 6U) {
        return;
    }
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    if (two_digits(&field[0], &hour) && two_digits(&field[2], &minute) &&
        two_digits(&field[4], &second) && (hour < 24U) && (minute < 60U) && (second < 61U)) {
        out->has_time = true;
        out->hour = hour;
        out->minute = minute;
        out->second = second;
    }
}

static void parse_date(const char *field, nmea_sentence_t *out)
{
    if (strlen(field) != 6U) {
        return;
    }
    uint8_t day = 0;
    uint8_t month = 0;
    uint8_t year = 0;
    if (two_digits(&field[0], &day) && two_digits(&field[2], &month) &&
        two_digits(&field[4], &year) && (day >= 1U) && (day <= 31U) && (month >= 1U) &&
        (month <= 12U)) {
        out->has_date = true;
        out->day = day;
        out->month = month;
        out->year = (uint16_t)(NMEA_YEAR_BASE + year);
    }
}

static void parse_position(const char *body, size_t body_len, uint32_t idx_lat,
                           nmea_sentence_t *out)
{
    char lat[NMEA_FIELD_MAX];
    char ns[NMEA_FIELD_MAX];
    char lon[NMEA_FIELD_MAX];
    char ew[NMEA_FIELD_MAX];

    if (get_field(body, body_len, idx_lat, lat) && get_field(body, body_len, idx_lat + 1U, ns) &&
        get_field(body, body_len, idx_lat + 2U, lon) &&
        get_field(body, body_len, idx_lat + 3U, ew) && parse_coord(lat, ns, 90.0, &out->lat) &&
        parse_coord(lon, ew, 180.0, &out->lon)) {
        out->has_position = true;
    }
}

static fw_err_t parse_gga(const char *body, size_t body_len, nmea_sentence_t *out)
{
    char field[NMEA_FIELD_MAX];
    float value = 0.0f;

    if (count_fields(body, body_len) < GGA_MIN_FIELDS) {
        return FW_ERR_INVALID_DATA;
    }

    if (get_field(body, body_len, GGA_IDX_TIME, field)) {
        parse_time(field, out);
    }
    parse_position(body, body_len, GGA_IDX_LAT, out);

    if (get_field(body, body_len, GGA_IDX_QUALITY, field) && (field[0] >= '1') &&
        (field[0] <= '9')) {
        out->fix_valid = out->has_position;
    }
    if (get_field(body, body_len, GGA_IDX_SATS, field) && parse_float(field, &value) &&
        (value >= 0.0f) && (value <= 255.0f)) {
        out->has_sats = true;
        out->sats = (uint8_t)value;
    }
    if (get_field(body, body_len, GGA_IDX_HDOP, field) && parse_float(field, &value)) {
        out->has_hdop = true;
        out->hdop = value;
    }
    if (get_field(body, body_len, GGA_IDX_ALT, field) && parse_float(field, &value)) {
        out->has_alt = true;
        out->alt_m = value;
    }
    return FW_OK;
}

static fw_err_t parse_rmc(const char *body, size_t body_len, nmea_sentence_t *out)
{
    char field[NMEA_FIELD_MAX];
    float value = 0.0f;

    if (count_fields(body, body_len) < RMC_MIN_FIELDS) {
        return FW_ERR_INVALID_DATA;
    }

    if (get_field(body, body_len, RMC_IDX_TIME, field)) {
        parse_time(field, out);
    }
    if (get_field(body, body_len, RMC_IDX_DATE, field)) {
        parse_date(field, out);
    }
    parse_position(body, body_len, RMC_IDX_LAT, out);

    if (get_field(body, body_len, RMC_IDX_STATUS, field) && (field[0] == 'A')) {
        out->fix_valid = out->has_position;
    }
    if (get_field(body, body_len, RMC_IDX_SPEED, field) && parse_float(field, &value)) {
        out->has_speed = true;
        out->speed_kmh = value * KNOTS_TO_KMH;
    }
    if (get_field(body, body_len, RMC_IDX_COURSE, field) && parse_float(field, &value)) {
        out->has_course = true;
        out->course_deg = value;
    }
    return FW_OK;
}

static fw_err_t parse_gsa(const char *body, size_t body_len, nmea_sentence_t *out)
{
    char field[NMEA_FIELD_MAX];
    float value = 0.0f;

    if (count_fields(body, body_len) < GSA_MIN_FIELDS) {
        return FW_ERR_INVALID_DATA;
    }
    if (get_field(body, body_len, GSA_IDX_FIX, field) && (field[0] != '\0')) {
        out->fix_valid = (atoi(field) > GSA_FIX_NONE);
    }
    if (get_field(body, body_len, GSA_IDX_HDOP, field) && parse_float(field, &value)) {
        out->has_hdop = true;
        out->hdop = value;
    }
    return FW_OK;
}

static fw_err_t parse_gsv(const char *body, size_t body_len, nmea_sentence_t *out)
{
    char field[NMEA_FIELD_MAX];
    float value = 0.0f;

    if (count_fields(body, body_len) < GSV_MIN_FIELDS) {
        return FW_ERR_INVALID_DATA;
    }
    if (get_field(body, body_len, GSV_IDX_IN_VIEW, field) && parse_float(field, &value) &&
        (value >= 0.0f) && (value <= 255.0f)) {
        out->has_sats_in_view = true;
        out->sats_in_view = (uint8_t)value;
    }
    return FW_OK;
}

static bool talker_ok(const char *body)
{
    static const char *const talkers[] = {"GP", "GN", "GL", "GA"};
    for (size_t i = 0; i < (sizeof(talkers) / sizeof(talkers[0])); i++) {
        if (strncmp(body, talkers[i], 2) == 0) {
            return true;
        }
    }
    return false;
}

fw_err_t nmea_parse_sentence(const char *line, nmea_sentence_t *out)
{
    if ((line == NULL) || (out == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }

    memset(out, 0, sizeof(*out));
    out->type = NMEA_SENTENCE_OTHER;

    size_t body_len = 0;
    if (!checksum_ok(line, &body_len) || (body_len < (NMEA_HEADER_LEN - 1U))) {
        return FW_ERR_INVALID_DATA;
    }

    const char *body = &line[1];
    if (!talker_ok(body)) {
        return FW_ERR_INVALID_DATA;
    }

    out->talker[0] = body[0];
    out->talker[1] = body[1];
    out->talker[2] = '\0';

    const char *type = &body[2];
    if (strncmp(type, "GGA", 3) == 0) {
        out->type = NMEA_SENTENCE_GGA;
        return parse_gga(body, body_len, out);
    }
    if (strncmp(type, "RMC", 3) == 0) {
        out->type = NMEA_SENTENCE_RMC;
        return parse_rmc(body, body_len, out);
    }
    if (strncmp(type, "GSA", 3) == 0) {
        out->type = NMEA_SENTENCE_GSA;
        return parse_gsa(body, body_len, out);
    }
    if (strncmp(type, "GSV", 3) == 0) {
        out->type = NMEA_SENTENCE_GSV;
        return parse_gsv(body, body_len, out);
    }
    return FW_OK; /* sentença válida que não usamos */
}
