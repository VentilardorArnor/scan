#include <string.h>

#include "app_bus.h"
#include "app_tasks.h"
#include "board_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gps_track.h"
#include "nmea.h"
#include "timekeeping.h"

/*
 * gps_task — única dona da UART do GPS. Lê bytes, monta e valida sentenças
 * NMEA, mantém a posição filtrada e oferece a hora do GPS ao relógio.
 */

#define GPS_RX_BUFFER_BYTES (2048)
#define GPS_READ_CHUNK      (128U)
#define GPS_READ_TIMEOUT_MS (200U)
#define GPS_NO_DATA_MS      (CONTRACT_GPS_NO_DATA_MS)
#define LOCK_TIMEOUT_MS     (50U)
#define DIAG_PERIOD_MS      (30000U)
#define TALKER_SLOTS        (3U) /* GP, GL, GA: satélites em vista por constelação */

/* Contadores da janela de diagnóstico (só a gps_task usa). */
typedef struct {
    uint32_t bytes;
    uint32_t lines;
    uint32_t valid;
    uint32_t invalid; /* checksum, formato ou talker não suportado */
    uint32_t gga;
    uint32_t rmc;
    uint32_t gsv;
    uint8_t in_view[TALKER_SLOTS];
    bool has_gga;
    uint8_t gga_quality_fix; /* 1 = GGA com fix */
    uint8_t gga_sats_used;
    float gga_hdop;
    bool has_utc;
} gps_diag_t;

static const char *TAG = "gps_task";

static SemaphoreHandle_t s_fix_lock;
static contract_gps_t s_fix; /* protegido por s_fix_lock */
static bool s_has_fix;       /* protegido por s_fix_lock */

static fw_err_t uart_setup(void)
{
    const uart_config_t config = {
        .baud_rate = BOARD_GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if ((uart_driver_install(BOARD_GPS_UART_NUM, GPS_RX_BUFFER_BYTES, 0, 0, NULL, 0) != ESP_OK) ||
        (uart_param_config(BOARD_GPS_UART_NUM, &config) != ESP_OK) ||
        (uart_set_pin(BOARD_GPS_UART_NUM, BOARD_GPS_TX_PIN, BOARD_GPS_RX_PIN, UART_PIN_NO_CHANGE,
                      UART_PIN_NO_CHANGE) != ESP_OK)) {
        return FW_ERR_IO;
    }
    return FW_OK;
}

/* Copia o resultado do filtro para o que as outras tarefas enxergam. */
static void publish_fix(const gps_track_t *track, uint64_t now_ms, bool *had_fix)
{
    gps_fix_t fix;
    const bool has_fix = gps_track_get_fix(track, now_ms, &fix);

    if (xSemaphoreTake(s_fix_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) == pdTRUE) {
        s_has_fix = has_fix;
        if (has_fix) {
            s_fix = (contract_gps_t){
                .lat = fix.lat,
                .lon = fix.lon,
                .has_hdop = fix.has_hdop,
                .hdop = fix.hdop,
                .has_sats = fix.has_sats,
                .sats = fix.sats,
                .has_alt = fix.has_alt,
                .alt_m = fix.alt_m,
                .has_speed = fix.has_speed,
                .speed_kmh = fix.speed_kmh,
                .has_course = fix.has_course,
                .course_deg = fix.course_deg,
            };
        }
        (void)xSemaphoreGive(s_fix_lock);
    }

    if (has_fix && !*had_fix) {
        app_bus_post_event(CONTRACT_EV_GPS_FIX_ACQUIRED, "{\"sats\":%u}", (unsigned)fix.sats);
    } else if (!has_fix && *had_fix) {
        app_bus_post_event(CONTRACT_EV_GPS_FIX_LOST, NULL);
    } else {
        /* sem mudança */
    }
    *had_fix = has_fix;
}

static int32_t talker_slot(const char *talker)
{
    static const char *const names[TALKER_SLOTS] = {"GP", "GL", "GA"};
    for (uint32_t i = 0; i < TALKER_SLOTS; i++) {
        if (strcmp(talker, names[i]) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Separa, no log, falha de comunicação com o módulo de ausência de fix. As
 * coordenadas saem arredondadas em 2 casas (~1 km): o valor exato vai só na
 * telemetria. */
static void log_diag(gps_diag_t *d, const gps_track_t *track, uint64_t now_ms, bool uart_ok)
{
    gps_fix_t fix;
    const bool has_fix = gps_track_get_fix(track, now_ms, &fix);
    const uint32_t in_view = (uint32_t)d->in_view[0] + d->in_view[1] + d->in_view[2];

    if (!uart_ok) {
        ESP_LOGE(TAG, "GPS: UART não configurada");
    } else if (d->bytes == 0U) {
        ESP_LOGW(TAG, "GPS: SEM COMUNICAÇÃO - 0 bytes em %u s na UART%d (RX = GPIO%d). "
                      "Confira alimentação do módulo, TX do GPS -> GPIO%d e GND comum",
                 (unsigned)(DIAG_PERIOD_MS / 1000U), BOARD_GPS_UART_NUM, BOARD_GPS_RX_PIN,
                 BOARD_GPS_RX_PIN);
    } else if (d->valid == 0U) {
        ESP_LOGW(TAG, "GPS: %u bytes, %u linhas, nenhuma sentença NMEA válida (%u inválidas). "
                      "Provável baud diferente de %d ou ruído na linha",
                 (unsigned)d->bytes, (unsigned)d->lines, (unsigned)d->invalid, BOARD_GPS_BAUD);
    } else if (!has_fix) {
        ESP_LOGI(TAG, "GPS: comunicação OK (%u sentenças válidas, %u inválidas em %u s; GGA %u, "
                      "RMC %u, GSV %u) | SEM FIX: satélites em vista %u (GP %u, GL %u, GA %u), "
                      "em uso %u | hora UTC do GPS: %s",
                 (unsigned)d->valid, (unsigned)d->invalid, (unsigned)(DIAG_PERIOD_MS / 1000U),
                 (unsigned)d->gga, (unsigned)d->rmc, (unsigned)d->gsv, (unsigned)in_view,
                 (unsigned)d->in_view[0], (unsigned)d->in_view[1], (unsigned)d->in_view[2],
                 (unsigned)(d->has_gga ? d->gga_sats_used : 0U), d->has_utc ? "sim" : "não");
    } else {
        ESP_LOGI(TAG, "GPS: FIX OK | em uso %u, em vista %u, HDOP %.1f, %u amostras na média | "
                      "posição ~ %.2f, %.2f (2 casas no log; completa na telemetria)",
                 (unsigned)(fix.has_sats ? fix.sats : 0U), (unsigned)in_view,
                 fix.has_hdop ? (double)fix.hdop : -1.0, (unsigned)fix.sample_count, fix.lat,
                 fix.lon);
    }

    const uint8_t keep_view[TALKER_SLOTS] = {d->in_view[0], d->in_view[1], d->in_view[2]};
    memset(d, 0, sizeof(*d));
    memcpy(d->in_view, keep_view, sizeof(keep_view)); /* GSV vem a cada ~1 s; mantém o último */
}

static void gps_task(void *arg)
{
    (void)arg;
    static gps_diag_t diag;
    uint64_t next_diag_ms = DIAG_PERIOD_MS;
    static uint8_t chunk[GPS_READ_CHUNK];
    static gps_track_t track;
    static nmea_framer_t framer;
    nmea_sentence_t sentence;
    bool had_fix = false;
    bool no_data_reported = false;
    uint64_t last_sentence_ms = timekeeping_mono_ms();

    gps_track_init(&track);
    nmea_framer_init(&framer);
    (void)esp_task_wdt_add(NULL);

    const bool uart_ok = (uart_setup() == FW_OK);
    if (!uart_ok) {
        ESP_LOGE(TAG, "falha ao configurar a UART do GPS");
    }

    for (;;) {
        int32_t got = 0;
        if (uart_ok) {
            got = uart_read_bytes(BOARD_GPS_UART_NUM, chunk, sizeof(chunk),
                                  pdMS_TO_TICKS(GPS_READ_TIMEOUT_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(GPS_READ_TIMEOUT_MS));
        }
        const uint64_t now_ms = timekeeping_mono_ms();

        if (got > 0) {
            diag.bytes += (uint32_t)got;
        }
        for (int32_t i = 0; i < got; i++) {
            if (!nmea_framer_feed(&framer, chunk[i])) {
                continue;
            }
            diag.lines++;
            /* Checksum inválido ou talker desconhecido: sentença ignorada. */
            if (nmea_parse_sentence(framer.line, &sentence) != FW_OK) {
                diag.invalid++;
                continue;
            }
            diag.valid++;
            if (sentence.type == NMEA_SENTENCE_GGA) {
                diag.gga++;
                diag.has_gga = true;
                diag.gga_sats_used = sentence.has_sats ? sentence.sats : 0U;
                diag.gga_hdop = sentence.has_hdop ? sentence.hdop : 0.0f;
            } else if (sentence.type == NMEA_SENTENCE_RMC) {
                diag.rmc++;
                diag.has_utc = diag.has_utc || (sentence.has_time && sentence.has_date);
            } else if (sentence.type == NMEA_SENTENCE_GSV) {
                diag.gsv++;
                const int32_t slot = talker_slot(sentence.talker);
                if ((slot >= 0) && sentence.has_sats_in_view) {
                    diag.in_view[slot] = sentence.sats_in_view;
                }
            } else {
                /* GSA e outras: contam só como válidas */
            }
            last_sentence_ms = now_ms;
            gps_track_feed(&track, &sentence, now_ms);

            int64_t utc = 0;
            uint64_t utc_at_ms = 0;
            if ((sentence.type == NMEA_SENTENCE_RMC) &&
                gps_track_get_utc(&track, &utc, &utc_at_ms) && (utc_at_ms == now_ms)) {
                timekeeping_offer_gps(utc, utc_at_ms);
            }
        }

        publish_fix(&track, now_ms, &had_fix);

        if (now_ms >= next_diag_ms) {
            log_diag(&diag, &track, now_ms, uart_ok);
            next_diag_ms = now_ms + DIAG_PERIOD_MS;
        }

        /* UART muda: cabo solto, módulo sem alimentação, TX/RX trocados. */
        const bool silent = (now_ms - last_sentence_ms) > GPS_NO_DATA_MS;
        if (silent && !no_data_reported) {
            app_bus_post_event(CONTRACT_EV_GPS_NO_DATA, "{\"silent_s\":%u}",
                               (unsigned)(GPS_NO_DATA_MS / 1000U));
            no_data_reported = true;
        } else if (!silent) {
            no_data_reported = false;
        } else {
            /* já avisado */
        }

        (void)esp_task_wdt_reset();
    }
}

void gps_task_start(void)
{
    s_fix_lock = xSemaphoreCreateMutex();
    configASSERT(s_fix_lock != NULL);
    const BaseType_t created =
        xTaskCreate(gps_task, "gps_task", APP_TASK_STACK_DEFAULT, NULL, APP_TASK_PRIO_GPS, NULL);
    configASSERT(created == pdPASS);
}

bool gps_task_get_fix(contract_gps_t *out)
{
    bool has_fix = false;
    if ((out == NULL) || (s_fix_lock == NULL)) {
        return false;
    }
    if (xSemaphoreTake(s_fix_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) == pdTRUE) {
        has_fix = s_has_fix;
        if (has_fix) {
            *out = s_fix;
        }
        (void)xSemaphoreGive(s_fix_lock);
    }
    return has_fix;
}
