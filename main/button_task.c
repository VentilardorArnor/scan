#include "app_tasks.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "identity.h"
#include "nvs.h"
#include "wifi_sta.h"

/*
 * Ações físicas pelo botão BOOT (GPIO0), com a placa JÁ ligada:
 *
 *   segurar 5 a 15 s e soltar  -> TROCA DE WI-FI: esquece a rede e reabre o
 *                                 modo de configuração BLE. A identidade
 *                                 (certificado) é preservada.
 *   segurar 15 s ou mais       -> RESET DE FÁBRICA: apaga Wi-Fi, identidade,
 *                                 E-stop e o journal. Depois disso a placa
 *                                 precisa de uma linha de provisionamento
 *                                 NOVA (o token antigo já foi usado).
 *
 * Apertar BOOT durante o reset/energização põe o chip em modo de gravação:
 * isso não é uma ação deste firmware.
 */

#define BUTTON_POLL_MS        (50U)
#define WIFI_RESET_HOLD_MS    (5000U)
#define FACTORY_RESET_HOLD_MS (15000U)
#define BUTTON_STACK          (3072U)
#define BUTTON_PRIO           (2U)

static const char *TAG = "botao";

static void erase_namespace(const char *ns)
{
    nvs_handle_t h = 0;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_all(h);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

static void wifi_reset(void)
{
    ESP_LOGW(TAG, "troca de Wi-Fi: rede esquecida; identidade preservada. Reiniciando");
    (void)wifi_sta_forget_network();
    esp_restart();
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "RESET DE FÁBRICA: apagando Wi-Fi, identidade, E-stop e journal. Reiniciando");
    (void)wifi_sta_forget_network();
    erase_namespace(IDENTITY_NAMESPACE);
    erase_namespace("ctl");
    erase_namespace("prov");
    journal_task_request_wipe(); /* o journal é formatado no próximo boot, antes de montar */
    esp_restart();
}

static void button_task(void *arg)
{
    (void)arg;
    uint32_t held_ms = 0;
    bool warned_wifi = false;
    bool warned_factory = false;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "botão indisponível");
        vTaskDelete(NULL);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        const bool pressed = (gpio_get_level(BOARD_BUTTON_PIN) == BOARD_BUTTON_ACTIVE_LEVEL);

        if (pressed) {
            held_ms += BUTTON_POLL_MS;
            if (!warned_wifi && (held_ms >= WIFI_RESET_HOLD_MS)) {
                warned_wifi = true;
                ESP_LOGW(TAG, "solte agora para TROCAR O WI-FI; continue até 15 s para RESET DE FÁBRICA");
            }
            if (!warned_factory && (held_ms >= FACTORY_RESET_HOLD_MS)) {
                warned_factory = true;
                ESP_LOGW(TAG, "solte para confirmar o RESET DE FÁBRICA");
            }
            continue;
        }

        if (held_ms >= FACTORY_RESET_HOLD_MS) {
            factory_reset();
        } else if (held_ms >= WIFI_RESET_HOLD_MS) {
            wifi_reset();
        } else {
            /* toque curto: ignorado */
        }
        held_ms = 0;
        warned_wifi = false;
        warned_factory = false;
    }
}

void button_task_start(void)
{
    const BaseType_t created =
        xTaskCreate(button_task, "button", BUTTON_STACK, NULL, BUTTON_PRIO, NULL);
    configASSERT(created == pdPASS);
}
