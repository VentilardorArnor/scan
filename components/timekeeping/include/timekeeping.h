#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Relógio de parede do dispositivo: NTP -> GPS -> RTC (ordem de preferência). */

typedef enum {
    TIMEKEEPING_SOURCE_NONE = 0,
    TIMEKEEPING_SOURCE_NTP,
    TIMEKEEPING_SOURCE_GPS,
    TIMEKEEPING_SOURCE_RTC, /* hora que sobreviveu a um reset por software */
} timekeeping_source_t;

/** @brief Verifica se o relógio já está válido (hora mantida de antes do reset). */
void timekeeping_init(void);

/** @brief Inicia o SNTP. Chamar depois de ter IP. Chamadas repetidas são ignoradas. */
void timekeeping_start_ntp(const char *server);

/** @brief true quando o relógio pode ser usado em `ts` e em expiração de comando. */
bool timekeeping_is_valid(void);

/** @brief De onde veio a hora atual. */
timekeeping_source_t timekeeping_get_source(void);

/** @brief Segundos desde a época Unix (só confiável se timekeeping_is_valid()). */
int64_t timekeeping_now(void);

/** @brief Relógio monotônico desde o boot, em ms. */
uint64_t timekeeping_mono_ms(void);

/**
 * @brief Oferece a hora UTC lida do GPS.
 *
 * É usada se não houver NTP recente; nunca atrasa um relógio já sincronizado
 * por NTP.
 *
 * @param epoch   Hora UTC da sentença RMC
 * @param at_mono_ms Instante monotônico em que a sentença chegou
 */
void timekeeping_offer_gps(int64_t epoch, uint64_t at_mono_ms);

/** @brief Força nova sincronização (a nuvem recusou um lote por clock_skew). */
void timekeeping_request_resync(void);
