#include "app_bus.h"
#include "app_tasks.h"
#include "board_config.h"
#include "contract.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "relay_hw.h"
#include "sdkconfig.h"
#include "timekeeping.h"
#include "wifi_sta.h"

/*
 * Sequência de partida.
 *
 * SEGURANÇA (ADR-001): estes relés só fecham contato seco nas entradas de
 * comando do painel original do pivô. O firmware assume que tudo pode falhar:
 * ao ligar, reiniciar, perder o broker ou travar, todos os relés ficam
 * DESLIGADOS. Nenhum estado de relé é restaurado depois de um reboot.
 */

static const char *TAG = "app_main";

static const int8_t RELAY_PINS[BOARD_RELAY_COUNT] = BOARD_RELAY_PINS;
static const int8_t INPUT_PINS[] = BOARD_INPUT_PINS;

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power_on"; /* como no exemplo de docs/MQTT.md */
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    default:
        return "unknown";
    }
}

static fw_err_t nvs_setup(void)
{
    /* Nunca apaga o NVS sozinho: ele guarda a chave e o certificado do enroll,
     * e apagá-los exige uma linha de provisionamento nova. Erro aqui fica no
     * log para correção manual (idf.py erase-flash é decisão de quem opera). */
    const esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS não iniciou (%s); identidade e config indisponíveis",
                 esp_err_to_name(err));
        return FW_ERR_IO;
    }
    return FW_OK;
}

void app_main(void)
{
    /* 1. Relés primeiro, antes de qualquer outra coisa: nível inativo no
     *    registrador e só então o pino vira saída. */
    bool relays_ok = true;
#if BOARD_HAS_RELAYS
    const relay_hw_config_t relay_config = {
        .relay_pins = RELAY_PINS,
        .relay_count = BOARD_RELAY_COUNT,
        .relay_active_level = (BOARD_RELAY_ACTIVE_LEVEL != 0),
        .input_pins = (BOARD_INPUT_COUNT > 0U) ? INPUT_PINS : NULL,
        .input_count = BOARD_INPUT_COUNT,
        .input_active_level = (BOARD_INPUT_ACTIVE_LEVEL != 0),
    };
    relays_ok = (relay_hw_init(&relay_config, relay_task_on_pulse_done) == FW_OK);
    if (!relays_ok) {
        ESP_LOGE(TAG, "falha na inicialização dos relés");
        relay_hw_all_off();
    }
#endif

    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "pivo-fw %s, reset: %s, relés: %u (%s)", app_bus_fw_version(),
             reset_reason_str(reason), (unsigned)((BOARD_HAS_RELAYS != 0) ? BOARD_RELAY_COUNT : 0U),
             (REMOTE_CONTROL_ENABLED != 0) ? "controle remoto LIGADO"
                                           : "sempre abertos, comandos recusados");

    /* 2. Serviços de base. Falha aqui não liga relé nenhum; o firmware segue
     *    para pelo menos manter o fail-safe e os logs. */
    if (nvs_setup() != FW_OK) {
        ESP_LOGE(TAG, "NVS indisponível");
    }
    timekeeping_init();
    if (app_bus_init() != FW_OK) { /* gera o boot_id: precisa ser antes do Wi-Fi */
        ESP_LOGE(TAG, "sem memória para as filas; reiniciando");
        esp_restart();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Eventos de diagnóstico da partida (publicados quando houver conexão). */
    app_bus_post_event(CONTRACT_EV_BOOT, "{\"reset_reason\":\"%s\",\"fw\":\"%s\"}",
                       reset_reason_str(reason), app_bus_fw_version());
    if (reason == ESP_RST_BROWNOUT) {
        app_bus_post_event(CONTRACT_EV_BROWNOUT, NULL);
    }

    /* 4. Tarefas. A relay_task sobe primeiro para o fail-safe já estar ativo. */
#if BOARD_HAS_RELAYS
    relay_task_start();
    cmd_task_start();
#endif
    provision_task_start(); /* termina sozinha se o enroll já foi feito */
    gps_task_start();
    journal_task_start();
    mqtt_task_start();
    health_task_start(relays_ok);
    button_task_start();

    /* 5. Rede por último: Wi-Fi gravado ou modo de configuração BLE. Sem rede,
     *    o GPS e o journal continuam funcionando. */
    if (wifi_sta_start() != FW_OK) {
        ESP_LOGE(TAG, "rede não iniciada");
    }
}
