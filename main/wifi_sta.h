#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fw_err.h"

/*
 * Rede: Wi-Fi em modo estação, configurado pelo celular via BLE
 * (network_provisioning, segurança 2: SRP6a + AES-GCM, com a prova de posse
 * impressa na etiqueta). Ver docs/ble-provisionamento.md.
 *
 * - Sem Wi-Fi configurado: abre o BLE e espera o celular.
 * - Com Wi-Fi configurado: conecta direto, sem BLE, e reconecta sozinho com
 *   espera progressiva (2 s, 4 s, ... até 60 s).
 * - SSID e senha ficam no NVS do driver Wi-Fi e nunca vão para o log.
 */

#define WIFI_BLE_NAME_PREFIX "AGRO_"
#define WIFI_SEC2_USERNAME   "wifiprov" /* padrão dos apps/bibliotecas da Espressif */

/** @brief Inicia o Wi-Fi ou o modo de configuração BLE. Exige esp_netif e o loop de eventos. */
fw_err_t wifi_sta_start(void);

/** @brief true enquanto há endereço IP. */
bool wifi_sta_has_ip(void);

/** @brief RSSI do AP associado; false se não está associado. */
bool wifi_sta_get_rssi(int32_t *rssi);

/**
 * @brief Esquece a rede Wi-Fi gravada (troca de rede). Não toca na identidade.
 *
 * Depois disso, o próximo boot abre o modo de configuração BLE.
 */
fw_err_t wifi_sta_forget_network(void);
