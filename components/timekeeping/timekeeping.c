#include "timekeeping.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#define VALID_EPOCH_MIN      (1735689600LL) /* 2025-01-01: antes disso o relógio não foi acertado */
#define NTP_FRESH_MS         (86400000ULL)  /* NTP com menos de 24 h manda sobre o GPS */
#define GPS_MIN_CORRECTION_S (2LL)          /* não mexe no relógio por diferença menor */
#define MS_PER_S             (1000ULL)
#define US_PER_MS            (1000LL)

static const char *TAG = "timekeeping";

static atomic_int s_source;
static atomic_bool s_ntp_started;
static atomic_uint_fast64_t s_ntp_synced_at_ms;

static void ntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    atomic_store(&s_ntp_synced_at_ms, timekeeping_mono_ms());
    atomic_store(&s_source, (int)TIMEKEEPING_SOURCE_NTP);
    ESP_LOGI(TAG, "relógio sincronizado por NTP");
}

void timekeeping_init(void)
{
    /* O relógio do sistema sobrevive a reset por software e a pânico; depois
     * de falta de energia ele volta a 1970 e fica inválido. */
    atomic_store(&s_source, (timekeeping_now() >= VALID_EPOCH_MIN) ? (int)TIMEKEEPING_SOURCE_RTC
                                                                   : (int)TIMEKEEPING_SOURCE_NONE);
}

void timekeeping_start_ntp(const char *server)
{
    if ((server == NULL) || atomic_exchange(&s_ntp_started, true)) {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    config.sync_cb = ntp_sync_cb;
    if (esp_netif_sntp_init(&config) != ESP_OK) {
        atomic_store(&s_ntp_started, false);
        ESP_LOGE(TAG, "falha ao iniciar o SNTP");
    }
}

bool timekeeping_is_valid(void)
{
    return (atomic_load(&s_source) != (int)TIMEKEEPING_SOURCE_NONE) &&
           (timekeeping_now() >= VALID_EPOCH_MIN);
}

timekeeping_source_t timekeeping_get_source(void)
{
    return (timekeeping_source_t)atomic_load(&s_source);
}

int64_t timekeeping_now(void)
{
    return (int64_t)time(NULL);
}

uint64_t timekeeping_mono_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / US_PER_MS);
}

void timekeeping_offer_gps(int64_t epoch, uint64_t at_mono_ms)
{
    if (epoch < VALID_EPOCH_MIN) {
        return; /* receptor ainda sem data válida */
    }

    const uint64_t now_ms = timekeeping_mono_ms();
    const bool ntp_fresh = (atomic_load(&s_source) == (int)TIMEKEEPING_SOURCE_NTP) &&
                           ((now_ms - atomic_load(&s_ntp_synced_at_ms)) < NTP_FRESH_MS);
    if (ntp_fresh) {
        return;
    }

    /* Corrige pelo tempo que a sentença esperou até chegar aqui. */
    const int64_t gps_now = epoch + (int64_t)((now_ms - at_mono_ms) / MS_PER_S);
    const bool already_gps = (atomic_load(&s_source) == (int)TIMEKEEPING_SOURCE_GPS);
    if (already_gps && (llabs(gps_now - timekeeping_now()) < GPS_MIN_CORRECTION_S)) {
        return;
    }

    const struct timeval tv = {.tv_sec = (time_t)gps_now, .tv_usec = 0};
    if (settimeofday(&tv, NULL) == 0) {
        if (!already_gps) {
            ESP_LOGI(TAG, "relógio acertado pelo GPS");
        }
        atomic_store(&s_source, (int)TIMEKEEPING_SOURCE_GPS);
    }
}

void timekeeping_request_resync(void)
{
    if (atomic_load(&s_ntp_started)) {
        (void)esp_sntp_restart();
    }
}
