#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Estado de alto nível da placa, na ordem do fluxo de configuração. Vai para
 * o log a cada mudança e para o celular pelo endpoint BLE `agro-status`.
 * Nenhum segredo passa por aqui.
 */
typedef enum {
    DEVICE_STATE_BOOT = 0,
    DEVICE_STATE_AWAITING_CONFIG,     /* BLE aberto, sem Wi-Fi configurado */
    DEVICE_STATE_WIFI_CONNECTING,
    DEVICE_STATE_WIFI_BAD_PASSWORD,   /* autenticação recusada pelo roteador */
    DEVICE_STATE_WIFI_NOT_FOUND,      /* rede não encontrada */
    DEVICE_STATE_NO_INTERNET,         /* tem IP, mas não alcança NTP/servidor */
    DEVICE_STATE_PROVISIONING_ID,     /* enroll em andamento */
    DEVICE_STATE_ID_REFUSED,          /* enroll 404/410/422: precisa de linha nova */
    DEVICE_STATE_NO_FACTORY_DATA,     /* firmware sem linha de provisionamento */
    DEVICE_STATE_MQTT_CONNECTING,
    DEVICE_STATE_ONLINE,
} device_state_t;

/** @brief Muda o estado (thread-safe) e registra no log se mudou. */
void device_state_set(device_state_t state);

/** @brief Estado atual. */
device_state_t device_state_get(void);

/** @brief Nome estável do estado, em snake_case (contrato BLE). */
const char *device_state_name(device_state_t state);
