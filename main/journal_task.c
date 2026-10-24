#include <stdatomic.h>
#include <string.h>

#include "app_bus.h"
#include "app_tasks.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "journal.h"
#include "nvs.h"
#include "telemetry_tx.h"
#include "timekeeping.h"
#include "wifi_sta.h"

/*
 * journal_task — única dona do journal durável (LittleFS). Gera o envelope de
 * telemetria no intervalo configurado, grava antes de enviar e só libera com
 * ACK de aplicação da nuvem (ADR-002).
 */

#define JOURNAL_MOUNT_POINT     "/journal"
#define JOURNAL_PARTITION       "journal"
#define JOURNAL_MAX_BYTES       (6U * 1024U * 1024U) /* partição de ~7,8 MB, com folga para o LittleFS */
#define JOURNAL_SEGMENT_BYTES   (64U * 1024U)
#define JOURNAL_QUEUE_LEN       (32U) /* rajada de ACKs após reconectar: 8 da janela + reenvios */
#define JOURNAL_TICK_MS         (500U)
#define JOURNAL_POLL_MS         (5000U) /* reenvio por timeout é verificado neste passo */
#define ACK_JSON_MAX            (255U)
#define MS_PER_S                (1000ULL)
#define TIME_UNSYNCED_AFTER_MS  (60000U)
#define STATS_PERIOD_MS         (60000U)

typedef enum {
    JOURNAL_MSG_ACK = 0,
    JOURNAL_MSG_CONNECTED,
} journal_msg_kind_t;

typedef struct {
    journal_msg_kind_t kind;
    uint16_t len;
    char json[ACK_JSON_MAX + 1U];
} journal_msg_t;

static const char *TAG = "journal_task";

static QueueHandle_t s_queue;
static journal_t s_journal;   /* só a journal_task usa */
static telemetry_tx_t s_tx;   /* só a journal_task usa */
static char s_payload[JOURNAL_PAYLOAD_MAX + 1U];
static uint32_t s_seq;
static atomic_bool s_healthy;

static bool tx_publish(void *ctx, const char *payload, size_t len)
{
    (void)ctx;
    return app_bus_publish(MQTT_LINK_TOPIC_TELEMETRY, false, payload, len);
}

static void tx_event(void *ctx, telemetry_tx_event_t event, const journal_key_t *key)
{
    (void)ctx;
    switch (event) {
    case TELEMETRY_TX_EV_REJECTED_INVALID:
        app_bus_post_event(CONTRACT_EV_TELEMETRY_REJECTED,
                           "{\"reason\":\"%s\",\"boot_id\":\"%s\",\"seq\":%u}",
                           CONTRACT_ACK_REASON_INVALID, key->boot_id, (unsigned)key->seq);
        break;
    case TELEMETRY_TX_EV_CLOCK_SKEW:
        ESP_LOGW(TAG, "lote recusado por clock_skew; ressincronizando o relógio");
        timekeeping_request_resync();
        break;
    case TELEMETRY_TX_EV_UNCLAIMED:
        ESP_LOGW(TAG, "dispositivo ainda sem claim; telemetria mantida no journal");
        break;
    case TELEMETRY_TX_EV_ACCEPTED:
        ESP_LOGI(TAG, "telemetria aceita pelo servidor (stored/duplicate)");
        break;
    case TELEMETRY_TX_EV_REVOKED:
        ESP_LOGE(TAG, "dispositivo revogado; telemetria parada até reiniciar");
        break;
    default:
        break;
    }
}

static contract_time_source_t time_source(void)
{
    switch (timekeeping_get_source()) {
    case TIMEKEEPING_SOURCE_NTP:
        return CONTRACT_TIME_NTP;
    case TIMEKEEPING_SOURCE_GPS:
        return CONTRACT_TIME_GPS;
    case TIMEKEEPING_SOURCE_RTC:
        return CONTRACT_TIME_RTC;
    case TIMEKEEPING_SOURCE_NONE:
    default:
        return CONTRACT_TIME_NONE;
    }
}

/* Monta um envelope e grava no journal. A gravação acontece com ou sem rede. */
static void record_telemetry(void)
{
    contract_telemetry_t telemetry = {
        .ts = timekeeping_now(),
        .fw = app_bus_fw_version(),
        .boot_id = app_bus_boot_id(),
        .seq = s_seq,
        .time_source = time_source(),
        .heap_free = esp_get_free_heap_size(),
        .journal_pending = s_journal.pending,
    };
    telemetry.has_rssi = wifi_sta_get_rssi(&telemetry.rssi);
    /* Sem fix o objeto gps é omitido; nunca vai 0,0. */
    telemetry.has_gps = gps_task_get_fix(&telemetry.gps);
    static bool s_had_gps;
    if (telemetry.has_gps != s_had_gps) {
        ESP_LOGI(TAG, "telemetria %s o objeto gps (seq %u)",
                 telemetry.has_gps ? "passou a incluir" : "deixou de incluir", (unsigned)s_seq);
        s_had_gps = telemetry.has_gps;
    }

    if (contract_build_telemetry(&telemetry, s_payload, sizeof(s_payload)) != FW_OK) {
        ESP_LOGE(TAG, "falha ao serializar a telemetria");
        return;
    }

    journal_key_t key;
    memset(&key, 0, sizeof(key));
    (void)strncpy(key.boot_id, app_bus_boot_id(), JOURNAL_BOOT_ID_LEN);
    key.seq = s_seq;

    uint32_t dropped = 0;
    const fw_err_t err = journal_append(&s_journal, &key, s_payload, strlen(s_payload), &dropped);
    if (dropped > 0U) {
        app_bus_post_event(CONTRACT_EV_JOURNAL_FULL, "{\"dropped\":%u,\"dropped_total\":%u}",
                           (unsigned)dropped, (unsigned)s_journal.dropped);
    }
    if (err != FW_OK) {
        ESP_LOGE(TAG, "falha ao gravar no journal (%d)", (int)err);
        return; /* o seq não avança: o próximo ciclo reutiliza o número */
    }
    s_seq++;
}

#define NVS_NS_SYS            "sys"
#define NVS_KEY_WIPE_JOURNAL  "wipe_journal"

void journal_task_request_wipe(void)
{
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_SYS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, NVS_KEY_WIPE_JOURNAL, 1U);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

/* Reset de fábrica: os lotes do dono anterior não podem ir para o próximo. */
static void wipe_if_requested(void)
{
    nvs_handle_t h = 0;
    uint8_t wipe = 0;
    if (nvs_open(NVS_NS_SYS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if ((nvs_get_u8(h, NVS_KEY_WIPE_JOURNAL, &wipe) == ESP_OK) && (wipe != 0U)) {
        if (esp_littlefs_format(JOURNAL_PARTITION) == ESP_OK) {
            ESP_LOGW(TAG, "journal apagado (reset de fábrica)");
            (void)nvs_erase_key(h, NVS_KEY_WIPE_JOURNAL);
            (void)nvs_commit(h);
        }
    }
    nvs_close(h);
}

static bool storage_open(void)
{
    wipe_if_requested();

    const esp_vfs_littlefs_conf_t conf = {
        .base_path = JOURNAL_MOUNT_POINT,
        .partition_label = JOURNAL_PARTITION,
        .format_if_mount_failed = true, /* partição virgem na primeira partida */
        .dont_mount = false,
    };
    if (esp_vfs_littlefs_register(&conf) != ESP_OK) {
        ESP_LOGE(TAG, "falha ao montar o LittleFS do journal");
        return false;
    }
    if (journal_open(&s_journal, JOURNAL_MOUNT_POINT, JOURNAL_MAX_BYTES, JOURNAL_SEGMENT_BYTES) !=
        FW_OK) {
        ESP_LOGE(TAG, "falha ao abrir o journal");
        return false;
    }
    if (s_journal.corrupt_found > 0U) {
        app_bus_post_event(CONTRACT_EV_JOURNAL_CORRUPT, "{\"count\":%u}",
                           (unsigned)s_journal.corrupt_found);
    }
    ESP_LOGI(TAG, "journal aberto: %u pendentes, %u perdidos", (unsigned)s_journal.pending,
             (unsigned)s_journal.dropped);
    return true;
}

static void journal_task(void *arg)
{
    (void)arg;
    static journal_msg_t msg;
    contract_telemetry_ack_t ack;
    bool unsynced_reported = false;
    bool poll_now = true;
    uint64_t next_record_ms = 0;
    uint64_t next_poll_ms = 0;
    uint64_t next_stats_ms = STATS_PERIOD_MS;

    /* Montar (ou formatar, na primeira partida) e varrer o journal pode levar
     * vários segundos: a tarefa só entra no watchdog depois disso. */
    const bool ok = storage_open();
    atomic_store(&s_healthy, ok);
    (void)esp_task_wdt_add(NULL);
    if (ok) {
        telemetry_tx_init(&s_tx, &s_journal, tx_publish, tx_event, NULL);
        app_bus_set_journal_stats(s_journal.pending, s_journal.dropped);
    }

    for (;;) {
        const bool got = (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(JOURNAL_TICK_MS)) == pdTRUE);
        (void)esp_task_wdt_reset();
        if (!ok) {
            continue; /* sem armazenamento não há telemetria; o status informa o resto */
        }
        const uint64_t now_ms = timekeeping_mono_ms();

        if (got && (msg.kind == JOURNAL_MSG_ACK)) {
            if (contract_parse_telemetry_ack(msg.json, msg.len, &ack) == FW_OK) {
                telemetry_tx_on_ack(&s_tx, &ack, now_ms);
                poll_now = true;
            }
        } else if (got && (msg.kind == JOURNAL_MSG_CONNECTED)) {
            telemetry_tx_on_connect(&s_tx); /* reenvia os pendentes ao reconectar */
            poll_now = true;
        } else {
            /* só o tick */
        }

        if (now_ms >= next_stats_ms) {
            /* Diagnóstico do envio: sem isto, ACK que não chega e recusa sem
             * motivo conhecido seriam silenciosos. */
            const telemetry_tx_stats_t *st = &s_tx.stats;
            ESP_LOGI(TAG,
                     "tx: env %u reenv %u fila_cheia %u | ack stored %u dup %u invalid %u "
                     "unclaimed %u revoked %u skew %u outro %u sem_lote %u | espera %u s",
                     (unsigned)st->sent, (unsigned)st->resent, (unsigned)st->publish_refused,
                     (unsigned)st->ack_stored, (unsigned)st->ack_duplicate,
                     (unsigned)st->ack_invalid, (unsigned)st->ack_unclaimed,
                     (unsigned)st->ack_revoked, (unsigned)st->ack_clock_skew,
                     (unsigned)st->ack_unknown, (unsigned)st->ack_not_found,
                     (unsigned)((s_tx.hold_until_ms > now_ms) ? ((s_tx.hold_until_ms - now_ms) / 1000U)
                                                              : 0U));
            journal_iter_t it;
            journal_record_t head;
            memset(&head, 0, sizeof(head));
            journal_iter_begin(&s_journal, &it);
            const bool has_head = (journal_iter_next(&s_journal, &it, &head, NULL, 0) == FW_OK);
            journal_iter_end(&it);
            ESP_LOGI(TAG, "tx: ack_erro %u (ultimo %d) | acked %u | cursor seg %u off %u | 1o pendente %s/%u",
                     (unsigned)st->ack_other_err, (int)st->last_ack_err,
                     (unsigned)s_journal.acked_count, (unsigned)s_journal.cur_seg,
                     (unsigned)s_journal.cur_off, has_head ? head.key.boot_id : "-",
                     has_head ? (unsigned)head.key.seq : 0U);
            next_stats_ms = now_ms + STATS_PERIOD_MS;
        }

        if (now_ms >= next_record_ms) {
            if (timekeeping_is_valid()) {
                record_telemetry();
                poll_now = true;
                next_record_ms =
                    now_ms + ((uint64_t)app_bus_telemetry_interval_s() * MS_PER_S);
            } else if (!unsynced_reported && (now_ms > TIME_UNSYNCED_AFTER_MS)) {
                /* Sem hora não existe `ts` válido, então nenhum envelope é
                 * criado. Um fix de GPS traz a hora junto, logo não se perde
                 * posição por isso. */
                app_bus_post_event(CONTRACT_EV_TIME_UNSYNCED, NULL);
                unsynced_reported = true;
            } else {
                /* aguardando NTP ou GPS */
            }
        }

        if (poll_now || (now_ms >= next_poll_ms)) {
            telemetry_tx_poll(&s_tx, now_ms, mqtt_link_is_connected(), s_payload,
                              sizeof(s_payload));
            poll_now = false;
            next_poll_ms = now_ms + JOURNAL_POLL_MS;
        }

        app_bus_set_journal_stats(s_journal.pending, s_journal.dropped);
        (void)esp_task_wdt_reset();
    }
}

void journal_task_start(void)
{
    s_queue = xQueueCreate(JOURNAL_QUEUE_LEN, sizeof(journal_msg_t));
    configASSERT(s_queue != NULL);
    const BaseType_t created = xTaskCreate(journal_task, "journal_task", APP_TASK_STACK_LARGE,
                                           NULL, APP_TASK_PRIO_JOURNAL, NULL);
    configASSERT(created == pdPASS);
}

bool journal_task_post_ack(const char *json, size_t len)
{
    /* Roda na tarefa do cliente MQTT: a mensagem é montada na pilha (264 B). */
    journal_msg_t msg;
    if ((json == NULL) || (s_queue == NULL) || (len > ACK_JSON_MAX)) {
        return false;
    }
    msg.kind = JOURNAL_MSG_ACK;
    msg.len = (uint16_t)len;
    memcpy(msg.json, json, len);
    msg.json[len] = '\0';
    return xQueueSend(s_queue, &msg, 0) == pdTRUE;
}

void journal_task_notify_connected(void)
{
    journal_msg_t msg;
    if (s_queue == NULL) {
        return;
    }
    msg.kind = JOURNAL_MSG_CONNECTED;
    msg.len = 0;
    msg.json[0] = '\0';
    (void)xQueueSend(s_queue, &msg, 0);
}

bool journal_task_is_healthy(void)
{
    return atomic_load(&s_healthy);
}
