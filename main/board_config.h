#pragma once

/*
 * Mapa da placa: ESP32-S3-DevKitC-1 N16R8.
 *
 * Único lugar com números de pino. Evitados de propósito:
 *   0, 3, 45, 46  strapping
 *   19, 20        USB
 *   43, 44        console (UART0)
 *   26..37        flash/PSRAM do módulo N16R8
 * Confira o esquema da SUA placa antes de ligar qualquer coisa.
 */

/* Relés: o `channel` do contrato (0..15) é o índice nesta lista:
 * canal 0 -> GPIO4, canal 1 -> GPIO5, canal 2 -> GPIO6. Canal >= BOARD_RELAY_COUNT
 * é recusado com `unknown_channel`. Esta montagem tem 3 módulos HW-482
 * (relé JQC3F-5VDC-C, pinos S/+/-). Expansão prevista para até 8:
 * {4, 5, 6, 7, 15, 16, 17, 18} e (8U). */
#define BOARD_RELAY_PINS         {4, 5, 6}
#define BOARD_RELAY_COUNT        (3U)
/* Nível do GPIO que LIGA o relé. O HW-482 é ativo em HIGH: S em nível alto
 * fecha o relé (LED vermelho aceso). */
#define BOARD_RELAY_ACTIVE_LEVEL (1)

/* Entradas de confirmação do painel (INPUT_1..8). Nenhuma instalada.
 * Para usar: liste os GPIOs e ajuste a contagem, ex.: {8, 9} e (2U). */
#define BOARD_INPUT_PINS         {-1}
#define BOARD_INPUT_COUNT        (0U)
#define BOARD_INPUT_ACTIVE_LEVEL (0)

/* Botão BOOT da DevKitC (GPIO0, strapping): só é lido com a placa já ligada.
 * Ações em main/button_task.c. */
#define BOARD_BUTTON_PIN          (0)
#define BOARD_BUTTON_ACTIVE_LEVEL (0)

/* GPS (u-blox NEO-M8N / NEO-6M), NMEA 0183 */
#define BOARD_GPS_UART_NUM       (2)
#define BOARD_GPS_TX_PIN         (47) /* TX do ESP -> RX do GPS */
#define BOARD_GPS_RX_PIN         (21) /* RX do ESP <- TX do GPS */
#define BOARD_GPS_BAUD           (9600)
