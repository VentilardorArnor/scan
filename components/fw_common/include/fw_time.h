#pragma once

#include <stdint.h>

/**
 * @brief Converte data e hora civis UTC em segundos desde a época Unix.
 *
 * Calendário gregoriano proléptico; não valida faixas (o chamador valida).
 * Existe porque timegm() não é portável entre o host e a newlib do ESP-IDF.
 *
 * @return Segundos desde 1970-01-01T00:00:00Z
 */
int64_t fw_time_civil_to_epoch(int64_t year, uint32_t month, uint32_t day, uint32_t hour,
                               uint32_t minute, uint32_t second);
