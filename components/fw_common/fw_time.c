#include "fw_time.h"

#define SECONDS_PER_DAY    (86400LL)
#define SECONDS_PER_HOUR   (3600LL)
#define SECONDS_PER_MINUTE (60LL)
#define DAYS_PER_ERA       (146097LL)
#define DAYS_TO_UNIX_EPOCH (719468LL)
#define YEARS_PER_ERA      (400LL)

int64_t fw_time_civil_to_epoch(int64_t year, uint32_t month, uint32_t day, uint32_t hour,
                               uint32_t minute, uint32_t second)
{
    /* days_from_civil: o ano começa em março para o dia bissexto cair no fim. */
    int64_t y = year - ((month <= 2U) ? 1 : 0);
    const int64_t era = ((y >= 0) ? y : (y - (YEARS_PER_ERA - 1))) / YEARS_PER_ERA;
    const uint32_t yoe = (uint32_t)(y - (era * YEARS_PER_ERA));
    const uint32_t doy =
        (((153U * ((month > 2U) ? (month - 3U) : (month + 9U))) + 2U) / 5U) + day - 1U;
    const uint32_t doe = (yoe * 365U) + (yoe / 4U) - (yoe / 100U) + doy;
    const int64_t days = (era * DAYS_PER_ERA) + (int64_t)doe - DAYS_TO_UNIX_EPOCH;

    return (days * SECONDS_PER_DAY) + ((int64_t)hour * SECONDS_PER_HOUR) +
           ((int64_t)minute * SECONDS_PER_MINUTE) + (int64_t)second;
}
