#include "device_state.h"

#include <stdatomic.h>

#include "esp_log.h"

static const char *TAG = "estado";

static atomic_int s_state = DEVICE_STATE_BOOT;

const char *device_state_name(device_state_t state)
{
    switch (state) {
    case DEVICE_STATE_AWAITING_CONFIG:
        return "aguardando_configuracao";
    case DEVICE_STATE_WIFI_CONNECTING:
        return "conectando_wifi";
    case DEVICE_STATE_WIFI_BAD_PASSWORD:
        return "erro_senha_wifi";
    case DEVICE_STATE_WIFI_NOT_FOUND:
        return "rede_nao_encontrada";
    case DEVICE_STATE_NO_INTERNET:
        return "sem_internet";
    case DEVICE_STATE_PROVISIONING_ID:
        return "provisionando_identidade";
    case DEVICE_STATE_ID_REFUSED:
        return "identidade_recusada";
    case DEVICE_STATE_NO_FACTORY_DATA:
        return "sem_dados_de_fabrica";
    case DEVICE_STATE_MQTT_CONNECTING:
        return "conectando_mqtt";
    case DEVICE_STATE_ONLINE:
        return "online";
    case DEVICE_STATE_BOOT:
    default:
        return "iniciando";
    }
}

void device_state_set(device_state_t state)
{
    const int previous = atomic_exchange(&s_state, (int)state);
    if (previous != (int)state) {
        ESP_LOGI(TAG, "%s -> %s", device_state_name((device_state_t)previous),
                 device_state_name(state));
    }
}

device_state_t device_state_get(void)
{
    return (device_state_t)atomic_load(&s_state);
}
