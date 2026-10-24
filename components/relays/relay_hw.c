#include "relay_hw.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"

#define US_PER_MS (1000ULL)
#define CH_BIT(n)   ((uint8_t)(1U << (n)))

static relay_hw_config_t s_config;
static relay_hw_pulse_done_cb_t s_on_pulse_done;
static esp_timer_handle_t s_pulse_timers[RELAY_HW_MAX_CHANNELS];
static atomic_uint_fast8_t s_on_mask;
static bool s_initialized;

static uint32_t level_for(bool on)
{
    return (on == s_config.relay_active_level) ? 1U : 0U;
}

/* Escreve o GPIO e o espelho. Pode ser chamada da relay_task e da tarefa do
 * esp_timer; gpio_set_level e a máscara atômica são seguros nos dois casos. */
static void write_pin(uint8_t index, bool on)
{
    (void)gpio_set_level((gpio_num_t)s_config.relay_pins[index], level_for(on));
    if (on) {
        (void)atomic_fetch_or(&s_on_mask, CH_BIT(index));
    } else {
        (void)atomic_fetch_and(&s_on_mask, (uint_fast8_t)~CH_BIT(index));
    }
}

/* Fim de pulso: desliga aqui mesmo, sem depender de nenhuma tarefa do app. */
static void pulse_timer_cb(void *arg)
{
    const uint8_t index = (uint8_t)(uintptr_t)arg;
    write_pin(index, false);
    if (s_on_pulse_done != NULL) {
        s_on_pulse_done(index);
    }
}

/* esp_restart() passa por aqui: reinício por software também desliga tudo. */
static void shutdown_handler(void)
{
    relay_hw_all_off();
}

fw_err_t relay_hw_init(const relay_hw_config_t *config, relay_hw_pulse_done_cb_t on_pulse_done)
{
    if ((config == NULL) || (config->relay_pins == NULL) || (config->relay_count == 0U) ||
        (config->relay_count > RELAY_HW_MAX_CHANNELS) ||
        (config->input_count > RELAY_HW_MAX_INPUTS) ||
        ((config->input_count > 0U) && (config->input_pins == NULL))) {
        return FW_ERR_INVALID_PARAM;
    }

    s_config = *config;
    s_on_pulse_done = on_pulse_done;
    atomic_store(&s_on_mask, 0);

    fw_err_t result = FW_OK;

    for (uint8_t i = 0; i < s_config.relay_count; i++) {
        const gpio_num_t pin = (gpio_num_t)s_config.relay_pins[i];
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << (uint32_t)pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        /* Ordem que importa: o registrador de saída recebe o nível INATIVO
         * antes de o pino virar saída. Ao habilitar, ele já sai desligado. */
        if ((gpio_set_level(pin, level_for(false)) != ESP_OK) || (gpio_config(&io) != ESP_OK) ||
            (gpio_set_level(pin, level_for(false)) != ESP_OK)) {
            result = FW_ERR_IO;
        }
    }

    for (uint8_t i = 0; i < s_config.input_count; i++) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << (uint32_t)s_config.input_pins[i],
            .mode = GPIO_MODE_INPUT,
            /* Puxa para o nível INATIVO: entrada solta lê "não confirmado". */
            .pull_up_en = s_config.input_active_level ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
            .pull_down_en =
                s_config.input_active_level ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK) {
            result = FW_ERR_IO;
        }
    }

    for (uint8_t i = 0; i < s_config.relay_count; i++) {
        const esp_timer_create_args_t args = {
            .callback = pulse_timer_cb,
            .arg = (void *)(uintptr_t)i,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "relay_pulse",
        };
        if (esp_timer_create(&args, &s_pulse_timers[i]) != ESP_OK) {
            result = FW_ERR_NO_MEM;
        }
    }

    if (esp_register_shutdown_handler(shutdown_handler) != ESP_OK) {
        result = FW_ERR_STATE;
    }

    s_initialized = (result == FW_OK);
    return result;
}

fw_err_t relay_hw_write(uint8_t index, bool on)
{
    if (!s_initialized) {
        return FW_ERR_STATE;
    }
    if (index >= s_config.relay_count) {
        return FW_ERR_INVALID_PARAM;
    }
    (void)esp_timer_stop(s_pulse_timers[index]); /* erro = não havia pulso */
    write_pin(index, on);
    return FW_OK;
}

void relay_hw_all_off(void)
{
    /* Sem checar s_initialized de propósito: os pinos são desligados mesmo
     * que a criação dos timers tenha falhado. */
    for (uint8_t i = 0; i < s_config.relay_count; i++) {
        if (s_pulse_timers[i] != NULL) {
            (void)esp_timer_stop(s_pulse_timers[i]);
        }
        write_pin(i, false);
    }
}

fw_err_t relay_hw_pulse_start(uint8_t index, uint32_t ms)
{
    if (!s_initialized) {
        return FW_ERR_STATE;
    }
    if ((index >= s_config.relay_count) || (ms == 0U)) {
        return FW_ERR_INVALID_PARAM;
    }

    (void)esp_timer_stop(s_pulse_timers[index]);
    write_pin(index, true);
    if (esp_timer_start_once(s_pulse_timers[index], (uint64_t)ms * US_PER_MS) != ESP_OK) {
        write_pin(index, false); /* sem timer não há garantia de desligar */
        return FW_ERR_STATE;
    }
    return FW_OK;
}

uint8_t relay_hw_get_on_mask(void)
{
    return (uint8_t)atomic_load(&s_on_mask);
}

uint8_t relay_hw_get_relay_count(void)
{
    return s_config.relay_count;
}

uint8_t relay_hw_get_input_count(void)
{
    return s_config.input_count;
}

uint8_t relay_hw_read_inputs_mask(void)
{
    uint8_t mask = 0;
    for (uint8_t i = 0; i < s_config.input_count; i++) {
        const bool level = (gpio_get_level((gpio_num_t)s_config.input_pins[i]) != 0);
        if (level == s_config.input_active_level) {
            mask |= CH_BIT(i);
        }
    }
    return mask;
}
