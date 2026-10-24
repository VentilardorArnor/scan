#include <string.h>

#include "app_bus.h"
#include "app_tasks.h"
#include "contract_json.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_link.h"
#include "ota.h"
#include "timekeeping.h"
#include "wifi_sta.h"

/*
 * health_task — heartbeat de DeviceStatus (retido), métricas e confirmação da
 * imagem de OTA depois do autoteste.
 */

#define HEALTH_TICK_MS        (1000U)
#define STATUS_PERIOD_MS      (CONTRACT_DEFAULT_STATUS_INTERVAL_S * 1000U)
#define METRICS_PERIOD_MS     (60000U)
#define SELF_TEST_AFTER_MS    (30000U) /* tempo para as tarefas provarem que sobem */
#define ROLLBACK_RESTART_MS   (5000U)  /* deixa o evento ota_failed sair */
#define STATUS_BUF_LEN        (384U)
#define MS_PER_S              (1000ULL)

static const char *TAG = "health_task";

static TaskHandle_t s_task;
static bool s_hardware_ok;
static char s_status_buf[STATUS_BUF_LEN];

static void publish_status(void)
{
    contract_status_t status = {
        .ts = timekeeping_now(),
        .fw = app_bus_fw_version(),
        .boot_id = app_bus_boot_id(),
        .uptime_s = (uint32_t)(timekeeping_mono_ms() / MS_PER_S),
        .journal_pending = app_bus_journal_pending(),
        .journal_dropped = app_bus_journal_dropped(),
    };
    status.has_rssi = wifi_sta_get_rssi(&status.rssi);

    if (contract_build_status(&status, s_status_buf, sizeof(s_status_buf)) == FW_OK) {
        (void)app_bus_publish(MQTT_LINK_TOPIC_STATUS, true, s_status_buf, strlen(s_status_buf));
    }
}

static void health_task(void *arg)
{
    (void)arg;
    uint64_t next_status_ms = 0;
    uint64_t next_metrics_ms = METRICS_PERIOD_MS;
    uint64_t restart_at_ms = 0;
    bool self_test_done = false;

    (void)esp_task_wdt_add(NULL);

    for (;;) {
        /* Acorda no tick ou quando a mqtt_task avisa que conectou. */
        const bool connected_now = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HEALTH_TICK_MS)) > 0U);
        const uint64_t now_ms = timekeeping_mono_ms();

        if (mqtt_link_is_connected() && (connected_now || (now_ms >= next_status_ms))) {
            publish_status();
            next_status_ms = now_ms + STATUS_PERIOD_MS;
        }

        if (!self_test_done && (now_ms >= SELF_TEST_AFTER_MS)) {
            self_test_done = true;
            const bool ok = s_hardware_ok && journal_task_is_healthy();
            if (ota_confirm_running_image(ok) == FW_ERR_STATE) {
                /* Imagem nova reprovada: avisa e reinicia para o bootloader
                 * voltar à anterior. */
                app_bus_post_event(CONTRACT_EV_OTA_FAILED, "{\"reason\":\"self_test\"}");
                restart_at_ms = now_ms + ROLLBACK_RESTART_MS;
            } else if (!ok) {
                ESP_LOGE(TAG, "autoteste falhou (relés=%d journal=%d)", (int)s_hardware_ok,
                         (int)journal_task_is_healthy());
            } else {
                /* tudo certo */
            }
        }
        if ((restart_at_ms != 0U) && (now_ms >= restart_at_ms)) {
            esp_restart();
        }

        if (now_ms >= next_metrics_ms) {
            next_metrics_ms = now_ms + METRICS_PERIOD_MS;
            ESP_LOGI(TAG, "journal: %u pendentes, %u perdidos | heap livre %u (mínimo %u)",
                     (unsigned)app_bus_journal_pending(), (unsigned)app_bus_journal_dropped(),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
        }

        (void)esp_task_wdt_reset();
    }
}

void health_task_start(bool hardware_ok)
{
    s_hardware_ok = hardware_ok;
    const BaseType_t created = xTaskCreate(health_task, "health_task", APP_TASK_STACK_DEFAULT,
                                           NULL, APP_TASK_PRIO_HEALTH, &s_task);
    configASSERT(created == pdPASS);
}

void health_task_notify_connected(void)
{
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}
