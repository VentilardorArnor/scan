#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_err.h"

#define NMEA_LINE_MAX (128U) /* a norma limita em 82; folga para extensões */

typedef enum {
    NMEA_SENTENCE_OTHER = 0, /* checksum válido, tipo não usado */
    NMEA_SENTENCE_GGA,
    NMEA_SENTENCE_RMC,
    NMEA_SENTENCE_GSA,
    NMEA_SENTENCE_GSV, /* satélites em vista (diagnóstico de antena) */
} nmea_sentence_type_t;

typedef struct {
    nmea_sentence_type_t type;
    char talker[3]; /* "GP", "GN", "GL" ou "GA" */

    bool fix_valid; /* GGA: qualidade > 0; RMC: status 'A'; GSA: fix 2D/3D */

    bool has_position;
    double lat; /* graus decimais, sul negativo */
    double lon; /* graus decimais, oeste negativo */

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

    bool has_time; /* hora UTC */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    bool has_sats_in_view; /* só GSV: satélites em vista deste talker */
    uint8_t sats_in_view;

    bool has_date; /* só RMC */
    uint16_t year;
    uint8_t month;
    uint8_t day;
} nmea_sentence_t;

/** @brief Monta linhas NMEA a partir do fluxo de bytes da UART. */
typedef struct {
    char line[NMEA_LINE_MAX];
    size_t len;
    bool in_sentence;
} nmea_framer_t;

/** @brief Zera o montador de linhas. */
void nmea_framer_init(nmea_framer_t *framer);

/**
 * @brief Alimenta um byte recebido.
 *
 * @return true quando `framer->line` contém uma sentença completa ("$...*HH",
 *         terminada em NUL, sem CR/LF), pronta para nmea_parse_sentence()
 */
bool nmea_framer_feed(nmea_framer_t *framer, uint8_t byte);

/**
 * @brief Valida o checksum e interpreta uma sentença GGA, RMC ou GSA.
 *
 * Aceita os talkers GP, GN, GL e GA.
 *
 * @param line Sentença completa, começando em '$', com "*HH" no fim
 * @param out  Resultado
 *
 * @return FW_OK; FW_ERR_INVALID_DATA para checksum, formato ou talker inválido
 */
fw_err_t nmea_parse_sentence(const char *line, nmea_sentence_t *out);
