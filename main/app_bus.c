#include "app_bus.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "contract.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "timekeeping.h"

#define PUB_QUEUE_LEN    (8U)
#define EVENT_QUEUE_LEN  (16U)
#define BOOT_ID_BYTES    (8U)
#define BOOT_ID_STR_LEN  ((BOOT_ID_BYTES * 2U) + 1U)
#define LOCK_TIMEOUT_MS  (200U)

static QueueHandle_t s_pub_queue;
static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_pub_lock;
static app_bus_pub_t s_pub_scratch; /* protegido por s_pub_lock */
static char s_boot_id[BOOT_ID_STR_LEN];

static const uint32_t s_telemetry_interval_s = CONFIG_PIVO_TELEMETRY_INTERVAL_S;
static atomic_uint s_failsafe_s = CONTRACT_DEFAULT_FAILSAFE_S;
static atomic_uint s_journal_pending;
static atomic_uint s_journal_dropped;

fw_err_t app_bus_init(void)
{
    uint8_t random_bytes[BOOT_ID_BYTES];

    /* Sem rádio ligado o gerador é só pseudoaleatório. A fonte de entropia do
     * bootloader garante que dois boots não repitam o boot_id, que é metade
     * da identidade de cada lote (boot_id, seq). Precisa ser antes do Wi-Fi. */
    bootloader_random_enable();
    esp_fill_random(random_bytes, sizeof(random_bytes));
    bootloader_random_disable();

    for (uint32_t i = 0; i < BOOT_ID_BYTES; i++) {
        (void)snprintf(&s_boot_id[i * 2U], 3, "%02x", random_bytes[i]);
    }

    s_pub_queue = xQueueCreate(PUB_QUEUE_LEN, sizeof(app_bus_pub_t));
    s_event_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(app_bus_event_t));
    s_pub_lock = xSemaphoreCreateMutex();
    if ((s_pub_queue == NULL) || (s_event_queue == NULL) || (s_pub_lock == NULL)) {
        return FW_ERR_NO_MEM;
    }
    return FW_OK;
}

const char *app_bus_boot_id(void)
{
    return s_boot_id;
}

const char *app_bus_fw_version(void)
{
    return esp_app_get_description()->version;
}

bool app_bus_publish(mqtt_link_topic_t topic, bool retain, const char *payload, size_t len)
{
    if ((payload == NULL) || (len >= APP_BUS_PAYLOAD_MAX) || (s_pub_queue == NULL)) {
        return false;
    }
    if (xSemaphoreTake(s_pub_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }

    s_pub_scratch.topic = topic;
    s_pub_scratch.retain = retain;
    s_pub_scratch.len = (uint16_t)len;
    memcpy(s_pub_scratch.payload, payload, len);
    s_pub_scratch.payload[len] = '\0';
    const bool queued = (xQueueSend(s_pub_queue, &s_pub_scratch, 0) == pdTRUE);

    (void)xSemaphoreGive(s_pub_lock);
    return queued;
}

void app_bus_post_event(const char *type, const char *data_fmt, ...)
{
    app_bus_event_t event;

    if ((type == NULL) || (s_event_queue == NULL)) {
        return;
    }
    memset(&event, 0, sizeof(event));
    (void)snprintf(event.type, sizeof(event.type), "%s", type);
    event.at_ms = timekeeping_mono_ms();

    if (data_fmt != NULL) {
        va_list args;
        va_start(args, data_fmt);
        const int32_t n = vsnprintf(event.data, sizeof(event.data), data_fmt, args);
        va_end(args);
        if ((n < 0) || ((size_t)n >= sizeof(event.data))) {
            event.data[0] = '\0'; /* não coube: publica o evento sem `data` */
        }
    }

    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        /* Fila cheia (muito tempo sem conexão): sai o evento mais antigo. */
        app_bus_event_t oldest;
        (void)xQueueReceive(s_event_queue, &oldest, 0);
        (void)xQueueSend(s_event_queue, &event, 0);
    }
}

bool app_bus_take_publish(app_bus_pub_t *out, uint32_t timeout_ms)
{
    if ((out == NULL) || (s_pub_queue == NULL)) {
        return false;
    }
    return xQueueReceive(s_pub_queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool app_bus_take_event(app_bus_event_t *out)
{
    if ((out == NULL) || (s_event_queue == NULL)) {
        return false;
    }
    return xQueueReceive(s_event_queue, out, 0) == pdTRUE;
}

void app_bus_set_failsafe_s(uint32_t failsafe_s)
{
    atomic_store(&s_failsafe_s, failsafe_s);
}

uint32_t app_bus_telemetry_interval_s(void)
{
    return s_telemetry_interval_s;
}

uint32_t app_bus_failsafe_s(void)
{
    return atomic_load(&s_failsafe_s);
}

void app_bus_set_journal_stats(uint32_t pending, uint32_t dropped)
{
    atomic_store(&s_journal_pending, pending);
    atomic_store(&s_journal_dropped, dropped);
}

uint32_t app_bus_journal_pending(void)
{
    return atomic_load(&s_journal_pending);
}

uint32_t app_bus_journal_dropped(void)
{
    return atomic_load(&s_journal_dropped);
}
