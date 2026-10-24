#include "gps_track.h"

#include <string.h>

#include "fw_time.h"

static void aux_set(gps_track_aux_t *aux, float value, uint64_t now_ms)
{
    aux->value = value;
    aux->at_ms = now_ms;
    aux->valid = true;
}

static bool aux_fresh(const gps_track_aux_t *aux, uint64_t now_ms)
{
    return aux->valid && ((now_ms - aux->at_ms) <= GPS_TRACK_MAX_AGE_MS);
}

void gps_track_init(gps_track_t *track)
{
    if (track != NULL) {
        memset(track, 0, sizeof(*track));
    }
}

void gps_track_feed(gps_track_t *track, const nmea_sentence_t *sentence, uint64_t now_ms)
{
    if ((track == NULL) || (sentence == NULL)) {
        return;
    }

    if (sentence->type == NMEA_SENTENCE_GGA) {
        if (sentence->has_sats) {
            aux_set(&track->sats, (float)sentence->sats, now_ms);
        }
        if (sentence->has_alt && sentence->fix_valid) {
            aux_set(&track->alt_m, sentence->alt_m, now_ms);
        }

        /* Só o GGA gera amostra de posição: é ele que traz o HDOP junto. */
        const bool hdop_ok = !sentence->has_hdop || (sentence->hdop <= GPS_TRACK_MAX_HDOP);
        if (sentence->fix_valid && sentence->has_position && hdop_ok) {
            gps_track_sample_t *sample = &track->samples[track->next];
            sample->used = true;
            sample->lat = sentence->lat;
            sample->lon = sentence->lon;
            sample->has_hdop = sentence->has_hdop;
            sample->hdop = sentence->hdop;
            sample->at_ms = now_ms;
            track->next = (uint8_t)((track->next + 1U) % GPS_TRACK_SAMPLES);
        }
    } else if (sentence->type == NMEA_SENTENCE_RMC) {
        if (!sentence->fix_valid) {
            return;
        }
        if (sentence->has_speed) {
            aux_set(&track->speed_kmh, sentence->speed_kmh, now_ms);
        }
        if (sentence->has_course) {
            aux_set(&track->course_deg, sentence->course_deg, now_ms);
        }
        if (sentence->has_time && sentence->has_date) {
            track->utc_epoch =
                fw_time_civil_to_epoch((int64_t)sentence->year, sentence->month, sentence->day,
                                       sentence->hour, sentence->minute, sentence->second);
            track->utc_at_ms = now_ms;
            track->has_utc = true;
        }
    } else {
        /* GSA e demais: nada a acumular (o HDOP vem do GGA). */
    }
}

bool gps_track_get_fix(const gps_track_t *track, uint64_t now_ms, gps_fix_t *out)
{
    if ((track == NULL) || (out == NULL)) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    /* Média simples de lat/lon. Não trata o antimeridiano (±180°), que não
     * ocorre na área de operação do projeto. */
    double sum_lat = 0.0;
    double sum_lon = 0.0;
    for (uint8_t i = 0; i < GPS_TRACK_SAMPLES; i++) {
        const gps_track_sample_t *sample = &track->samples[i];
        if (!sample->used || ((now_ms - sample->at_ms) > GPS_TRACK_MAX_AGE_MS)) {
            continue;
        }
        sum_lat += sample->lat;
        sum_lon += sample->lon;
        if (sample->has_hdop && (!out->has_hdop || (sample->hdop > out->hdop))) {
            out->has_hdop = true;
            out->hdop = sample->hdop;
        }
        out->sample_count++;
    }

    if (out->sample_count == 0U) {
        return false;
    }

    out->lat = sum_lat / (double)out->sample_count;
    out->lon = sum_lon / (double)out->sample_count;

    if (aux_fresh(&track->sats, now_ms)) {
        out->has_sats = true;
        out->sats = (uint8_t)track->sats.value;
    }
    if (aux_fresh(&track->alt_m, now_ms)) {
        out->has_alt = true;
        out->alt_m = track->alt_m.value;
    }
    if (aux_fresh(&track->speed_kmh, now_ms)) {
        out->has_speed = true;
        out->speed_kmh = track->speed_kmh.value;
    }
    if (aux_fresh(&track->course_deg, now_ms)) {
        out->has_course = true;
        out->course_deg = track->course_deg.value;
    }
    return true;
}

bool gps_track_get_utc(const gps_track_t *track, int64_t *epoch, uint64_t *at_ms)
{
    if ((track == NULL) || (epoch == NULL) || (at_ms == NULL) || !track->has_utc) {
        return false;
    }
    *epoch = track->utc_epoch;
    *at_ms = track->utc_at_ms;
    return true;
}
