#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nmea.h"

#define GPS_TRACK_SAMPLES    (5U)     /* N da média móvel */
#define GPS_TRACK_MAX_HDOP   (5.0f)   /* acima disso a leitura é descartada */
#define GPS_TRACK_MAX_AGE_MS (10000U) /* leitura mais velha não entra na média */

typedef struct {
    bool used;
    double lat;
    double lon;
    bool has_hdop;
    float hdop;
    uint64_t at_ms;
} gps_track_sample_t;

typedef struct {
    float value;
    uint64_t at_ms;
    bool valid;
} gps_track_aux_t;

/** @brief Estado do filtro de posição; pertence a uma única tarefa. */
typedef struct {
    gps_track_sample_t samples[GPS_TRACK_SAMPLES];
    uint8_t next;
    gps_track_aux_t sats;
    gps_track_aux_t alt_m;
    gps_track_aux_t speed_kmh;
    gps_track_aux_t course_deg;
    bool has_utc;
    int64_t utc_epoch;
    uint64_t utc_at_ms;
} gps_track_t;

typedef struct {
    double lat;
    double lon;
    bool has_hdop;
    float hdop; /* pior HDOP entre as leituras usadas */
    bool has_sats;
    uint8_t sats;
    bool has_alt;
    float alt_m;
    bool has_speed;
    float speed_kmh;
    bool has_course;
    float course_deg;
    uint8_t sample_count;
} gps_fix_t;

/** @brief Zera o filtro. */
void gps_track_init(gps_track_t *track);

/**
 * @brief Incorpora uma sentença já validada.
 *
 * @param now_ms Relógio monotônico, em ms
 */
void gps_track_feed(gps_track_t *track, const nmea_sentence_t *sentence, uint64_t now_ms);

/**
 * @brief Posição filtrada: média das leituras válidas e recentes.
 *
 * @return false se não há nenhuma leitura utilizável (sem fix: o chamador
 *         deve omitir o objeto gps, nunca enviar 0,0)
 */
bool gps_track_get_fix(const gps_track_t *track, uint64_t now_ms, gps_fix_t *out);

/**
 * @brief Última hora UTC informada pelo receptor (RMC com fix válido).
 *
 * @param epoch Segundos desde a época Unix
 * @param at_ms Instante monotônico em que a sentença chegou
 *
 * @return false se o receptor ainda não informou data e hora válidas
 */
bool gps_track_get_utc(const gps_track_t *track, int64_t *epoch, uint64_t *at_ms);
