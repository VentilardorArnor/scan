#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fw_err.h"

/*
 * Acesso aos GPIOs dos relés e das entradas de confirmação.
 *
 * Regra de segurança: qualquer dúvida termina com o relé DESLIGADO. Este
 * módulo nunca liga nada por conta própria e não guarda estado entre boots.
 */

#define RELAY_HW_MAX_CHANNELS (8U)
#define RELAY_HW_MAX_INPUTS   (8U)

typedef struct {
    const int8_t *relay_pins; /* canal 1..N -> GPIO */
    uint8_t relay_count;
    bool relay_active_level; /* nível do GPIO que LIGA o relé */
    const int8_t *input_pins; /* entradas de confirmação (pode ser NULL) */
    uint8_t input_count;
    bool input_active_level;
} relay_hw_config_t;

/** @brief Chamado (na tarefa do esp_timer) quando um pulso termina. */
typedef void (*relay_hw_pulse_done_cb_t)(uint8_t index);

/**
 * @brief Configura os pinos com todos os relés desligados.
 *
 * Escreve o nível inativo ANTES de habilitar a saída, para o relé não piscar
 * no boot. Deve ser a primeira coisa do app_main().
 *
 * @param config   Mapa de pinos (os ponteiros precisam continuar válidos)
 * @param on_pulse_done Aviso de fim de pulso (pode ser NULL)
 */
fw_err_t relay_hw_init(const relay_hw_config_t *config, relay_hw_pulse_done_cb_t on_pulse_done);

/** @brief Liga ou desliga um canal (índice 0..N-1). Cancela pulso em curso. */
fw_err_t relay_hw_write(uint8_t index, bool on);

/** @brief Desliga todos os canais e cancela todos os pulsos. */
void relay_hw_all_off(void);

/**
 * @brief Liga o canal e arma um esp_timer que o desliga depois de `ms`.
 *
 * O desligamento não depende de nenhuma tarefa do aplicativo.
 */
fw_err_t relay_hw_pulse_start(uint8_t index, uint32_t ms);

/** @brief Máscara do que o firmware realmente escreveu nos GPIOs. */
uint8_t relay_hw_get_on_mask(void);

/** @brief Quantidade de relés instalados (canais 1..N). */
uint8_t relay_hw_get_relay_count(void);

/** @brief Quantidade de entradas de confirmação instaladas. */
uint8_t relay_hw_get_input_count(void);

/** @brief Leitura das entradas de confirmação (bit n = entrada n+1 ativa). */
uint8_t relay_hw_read_inputs_mask(void);
