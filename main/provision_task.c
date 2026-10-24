#include "app_tasks.h"
#include "device_state.h"
#include "enroll.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "identity.h"
#include "timekeeping.h"
#include "wifi_sta.h"

/*
 * provision_task — primeira conexão (enroll). Roda só enquanto não há
 * certificado: espera rede e hora válida (o TLS confere a validade do
 * certificado do servidor), chama o enroll e termina. Não entra no watchdog
 * porque fica bloqueada na requisição HTTPS.
 */

#if __has_include("provisioning.h")
#include "provisioning.h"
#endif
#ifndef AGRO_PROVISIONING
#define AGRO_PROVISIONING ""
#endif

#define WAIT_POLL_MS        (1000U)
#define RETRY_START_MS      (10000U)
#define RETRY_MAX_MS        (300000U) /* 5 min */
#define STOPPED_LOG_MS      (60000U)

static const char *TAG = "provision";

static void wait_network_and_time(void)
{
    while (!wifi_sta_has_ip() || !timekeeping_is_valid()) {
        vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
    }
}

/* 404/410/422: parar e avisar. Sem LED de status na placa (o LED do módulo
 * de relé só mostra o relé), o aviso fica no log. */
static void stop_forever(const char *why)
{
    for (;;) {
        ESP_LOGE(TAG, "%s", why);
        vTaskDelay(pdMS_TO_TICKS(STOPPED_LOG_MS));
    }
}

static void provision_task(void *arg)
{
    (void)arg;
    identity_t identity;
    uint32_t retry_ms = RETRY_START_MS;

    if (AGRO_PROVISIONING[0] == '\0') {
        /* Sem linha no firmware: só serve se o enroll já foi feito antes. */
        if (identity_load(&identity) == FW_OK) {
            identity_release(&identity);
            vTaskDelete(NULL);
        }
        /* Sem identidade e sem linha: a rede funciona, mas não há como entrar
         * na plataforma. O estado é reafirmado a cada vez que o Wi-Fi volta. */
        ESP_LOGE(TAG, "sem certificado e sem linha de provisionamento: crie main/provisioning.h "
                      "a partir de main/provisioning.h.example");
        for (;;) {
            if (wifi_sta_has_ip()) {
                device_state_set(DEVICE_STATE_NO_FACTORY_DATA);
            }
            vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
        }
    }

    for (;;) {
        wait_network_and_time();

        int32_t http_status = 0;
        if (identity_load(&identity) != FW_OK) {
            device_state_set(DEVICE_STATE_PROVISIONING_ID);
        } else {
            identity_release(&identity);
        }
        const enroll_result_t result = enroll_ensure(AGRO_PROVISIONING, &http_status);

        if ((result == ENROLL_DONE) && (identity_load(&identity) == FW_OK)) {
            ESP_LOGI(TAG, "identidade pronta: %s", identity.device_id);
            identity_release(&identity);
            vTaskDelete(NULL); /* a mqtt_task segue daqui */
        }
        if ((result == ENROLL_NEED_NEW_LINE) || (result == ENROLL_FIRMWARE_ERROR)) {
            device_state_set(DEVICE_STATE_ID_REFUSED);
        }
        if ((result == ENROLL_RETRY) && (http_status == 0)) {
            device_state_set(DEVICE_STATE_NO_INTERNET); /* não chegou ao servidor */
        }
        if (result == ENROLL_NEED_NEW_LINE) {
            stop_forever((http_status == 410)
                             ? "linha de provisionamento vencida (410): gere uma linha nova"
                             : "linha de provisionamento inválida, já usada ou placa cancelada "
                               "(404): gere uma linha nova");
        }
        if (result == ENROLL_FIRMWARE_ERROR) {
            stop_forever("enroll recusado por erro do firmware (CSR ou resposta); veja o log acima");
        }

        ESP_LOGW(TAG, "enroll: nova tentativa em %u s", (unsigned)(retry_ms / 1000U));
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        retry_ms = (retry_ms >= (RETRY_MAX_MS / 2U)) ? RETRY_MAX_MS : (retry_ms * 2U);
    }
}

void provision_task_start(void)
{
    const BaseType_t created = xTaskCreate(provision_task, "provision", APP_TASK_STACK_TLS, NULL,
                                           APP_TASK_PRIO_PROVISION, NULL);
    configASSERT(created == pdPASS);
}
