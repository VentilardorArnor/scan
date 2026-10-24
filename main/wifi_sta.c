#include "wifi_sta.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_state.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_srp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"
#include "prov_line.h"
#include "protocomm_ble.h"
#include "protocomm_security.h"
#include "psa/crypto.h"
#include "timekeeping.h"

#if __has_include("provisioning.h")
#include "provisioning.h"
#endif
#ifndef AGRO_BLE_POP
#define AGRO_BLE_POP ""
#endif
#ifndef AGRO_PROVISIONING
#define AGRO_PROVISIONING ""
#endif

#define RETRY_START_US      (2000000ULL)  /* 2 s */
#define RETRY_MAX_US        (60000000ULL) /* 60 s */
#define INTERNET_CHECK_US   (20000000ULL) /* sem hora 20 s depois do IP = sem internet */
#define PROV_CONN_ATTEMPTS  (3U)          /* tentativas antes de avisar falha ao celular */
#define POP_MIN_LEN         (8U)
#define SRP_SALT_LEN        (16)
#define SRP_VERIFIER_MAX    (512U)
#define SHA256_LEN          (32U)
#define BLE_NAME_LEN        (12U)         /* "AGRO_" + 6 hex + NUL */
#define STATUS_JSON_MAX     (256U)

#define NVS_NS_NET          "net"
#define NVS_KEY_WIFI_OK     "wifi_ok"      /* 0 = credencial nunca conectou; 1 = confirmada */
#define WIFI_OK_PENDING     (0U)
#define WIFI_OK_CONFIRMED   (1U)

#define NVS_NS_PROV         "prov"
#define NVS_KEY_POP_SHA     "pop_sha"
#define NVS_KEY_SALT        "salt"
#define NVS_KEY_VERIFIER    "verifier"

/* UUID do serviço GATT de provisionamento das placas SmartagroBr. O app filtra
 * a varredura por ele (além do prefixo do nome). */
static uint8_t s_service_uuid[16] = {0x5a, 0x67, 0x72, 0x6f, 0x42, 0x72, 0x2d, 0x70,
                                     0x69, 0x76, 0x6f, 0x2d, 0x70, 0x72, 0x6f, 0x76};

static const char *TAG = "wifi_sta";

static atomic_bool s_has_ip;
static atomic_bool s_provisioning;   /* BLE aberto */
static atomic_bool s_prov_failed;    /* credenciais recusadas: espera o celular mandar outras */
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_internet_timer;
static uint64_t s_retry_us = RETRY_START_US;
static char s_ble_name[BLE_NAME_LEN];
static char s_last_reason[24] = "";

/* Precisam viver até o fim do provisionamento (NETWORK_PROV_END). */
static uint8_t s_salt[SRP_SALT_LEN];
static uint8_t s_verifier[SRP_VERIFIER_MAX];
static network_prov_security2_params_t s_sec2;

/* ------------------------------------------------------------------ */
/* Prova de posse (SRP6a): sal e verificador calculados na placa      */
/* ------------------------------------------------------------------ */

/* O sal e o verificador são derivados da prova de posse e guardados no NVS.
 * Se a prova de posse mudar (placa regravada), são recalculados. A prova de
 * posse em si nunca vai para o log nem para o NVS. */
static fw_err_t load_sec2_params(void)
{
    const char *pop = AGRO_BLE_POP;
    uint8_t sha[SHA256_LEN];
    uint8_t stored_sha[SHA256_LEN];
    size_t sha_len = 0;

    if (strlen(pop) < POP_MIN_LEN) {
        return FW_ERR_NOT_FOUND;
    }
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)pop, strlen(pop), sha, sizeof(sha),
                         &sha_len) != PSA_SUCCESS) {
        return FW_ERR_STATE;
    }

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_PROV, NVS_READWRITE, &h) != ESP_OK) {
        return FW_ERR_IO;
    }

    size_t len = sizeof(stored_sha);
    size_t salt_len = sizeof(s_salt);
    size_t verifier_len = sizeof(s_verifier);
    const bool cached = (nvs_get_blob(h, NVS_KEY_POP_SHA, stored_sha, &len) == ESP_OK) &&
                        (len == SHA256_LEN) && (memcmp(stored_sha, sha, SHA256_LEN) == 0) &&
                        (nvs_get_blob(h, NVS_KEY_SALT, s_salt, &salt_len) == ESP_OK) &&
                        (nvs_get_blob(h, NVS_KEY_VERIFIER, s_verifier, &verifier_len) == ESP_OK);

    fw_err_t err = FW_OK;
    if (!cached) {
        char *salt = NULL;
        char *verifier = NULL;
        int vlen = 0;
        ESP_LOGI(TAG, "calculando o verificador SRP da prova de posse (só na primeira vez)");
        if ((esp_srp_gen_salt_verifier(WIFI_SEC2_USERNAME, (int)strlen(WIFI_SEC2_USERNAME), pop,
                                       (int)strlen(pop), &salt, SRP_SALT_LEN, &verifier,
                                       &vlen) != ESP_OK) ||
            (vlen <= 0) || ((size_t)vlen > sizeof(s_verifier))) {
            err = FW_ERR_STATE;
        } else {
            memcpy(s_salt, salt, SRP_SALT_LEN);
            memcpy(s_verifier, verifier, (size_t)vlen);
            salt_len = SRP_SALT_LEN;
            verifier_len = (size_t)vlen;
            if ((nvs_set_blob(h, NVS_KEY_SALT, s_salt, salt_len) != ESP_OK) ||
                (nvs_set_blob(h, NVS_KEY_VERIFIER, s_verifier, verifier_len) != ESP_OK) ||
                (nvs_set_blob(h, NVS_KEY_POP_SHA, sha, SHA256_LEN) != ESP_OK) ||
                (nvs_commit(h) != ESP_OK)) {
                ESP_LOGW(TAG, "verificador SRP não guardado; será recalculado no próximo boot");
            }
        }
        free(salt);
        free(verifier);
    }
    nvs_close(h);

    if (err == FW_OK) {
        s_sec2.salt = (const char *)s_salt;
        s_sec2.salt_len = (uint16_t)salt_len;
        s_sec2.verifier = (const char *)s_verifier;
        s_sec2.verifier_len = (uint16_t)verifier_len;
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Rede confirmada                                                     */
/* ------------------------------------------------------------------ */

/* O network_provisioning grava SSID/senha na flash ANTES de testar e só
 * apaga depois de 3 falhas. Se a energia cair nesse meio, a placa voltaria
 * "configurada" com uma senha que nunca funcionou e não reabriria o BLE.
 * Esta marca resolve: credencial recebida = pendente; primeiro IP = confirmada.
 * Marca ausente (placa configurada antes desta versão) vale como confirmada. */
static void set_wifi_ok(uint8_t value)
{
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_NET, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, NVS_KEY_WIFI_OK, value);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

static bool wifi_pending(void)
{
    nvs_handle_t h = 0;
    uint8_t value = WIFI_OK_CONFIRMED;
    if (nvs_open(NVS_NS_NET, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_KEY_WIFI_OK, &value);
        nvs_close(h);
    }
    return value == WIFI_OK_PENDING;
}

/* ------------------------------------------------------------------ */
/* Endpoint BLE `agro-status`                                          */
/* ------------------------------------------------------------------ */

/* Responde a qualquer escrita com o estado atual em JSON. Roda dentro da
 * sessão de segurança 2: só quem tem a prova de posse lê. Sem segredos. */
static esp_err_t status_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)inbuf;
    (void)inlen;
    (void)priv_data;

    prov_line_t line;
    const bool has_line = (prov_line_parse(AGRO_PROVISIONING, &line) == FW_OK);

    cJSON *root = cJSON_CreateObject();
    char *json = NULL;
    if (root != NULL) {
        (void)cJSON_AddNumberToObject(root, "v", 1);
        if (has_line) {
            (void)cJSON_AddStringToObject(root, "cn", line.cn);
        } else {
            (void)cJSON_AddNullToObject(root, "cn");
        }
        (void)cJSON_AddStringToObject(root, "fw", esp_app_get_description()->version);
        (void)cJSON_AddStringToObject(root, "state", device_state_name(device_state_get()));
        if (s_last_reason[0] != '\0') {
            (void)cJSON_AddStringToObject(root, "wifi_error", s_last_reason);
        }
        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }
    memset(&line, 0, sizeof(line)); /* apaga o token da pilha */

    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *outbuf = (uint8_t *)json; /* o protocomm libera com free() */
    *outlen = (ssize_t)strlen(json);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Eventos                                                             */
/* ------------------------------------------------------------------ */

static void schedule_retry(void)
{
    (void)esp_timer_stop(s_retry_timer);
    if (esp_timer_start_once(s_retry_timer, s_retry_us) == ESP_OK) {
        ESP_LOGI(TAG, "nova tentativa de Wi-Fi em %u s", (unsigned)(s_retry_us / 1000000ULL));
    }
    s_retry_us = (s_retry_us >= (RETRY_MAX_US / 2U)) ? RETRY_MAX_US : (s_retry_us * 2U);
}

static void retry_cb(void *arg)
{
    (void)arg;
    device_state_set(DEVICE_STATE_WIFI_CONNECTING);
    (void)esp_wifi_connect();
}

static void internet_check_cb(void *arg)
{
    (void)arg;
    if (atomic_load(&s_has_ip) && !timekeeping_is_valid()) {
        device_state_set(DEVICE_STATE_NO_INTERNET);
    }
}

static void on_disconnected(const wifi_event_sta_disconnected_t *event)
{
    atomic_store(&s_has_ip, false);
    (void)esp_timer_stop(s_internet_timer);

    const uint8_t reason = event->reason;
    const bool bad_password =
        (reason == WIFI_REASON_AUTH_FAIL) || (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) ||
        (reason == WIFI_REASON_HANDSHAKE_TIMEOUT) || (reason == WIFI_REASON_MIC_FAILURE) ||
        (reason == WIFI_REASON_CONNECTION_FAIL);
    const bool not_found = (reason == WIFI_REASON_NO_AP_FOUND) ||
                           (reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY) ||
                           (reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD) ||
                           (reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD);

    if (bad_password) {
        (void)snprintf(s_last_reason, sizeof(s_last_reason), "auth_error");
        device_state_set(DEVICE_STATE_WIFI_BAD_PASSWORD);
    } else if (not_found) {
        (void)snprintf(s_last_reason, sizeof(s_last_reason), "ap_not_found");
        device_state_set(DEVICE_STATE_WIFI_NOT_FOUND);
    } else {
        device_state_set(DEVICE_STATE_WIFI_CONNECTING);
    }
    ESP_LOGW(TAG, "Wi-Fi desconectado (motivo %u)", (unsigned)reason);

    if (atomic_load(&s_provisioning)) {
        /* Durante a configuração o gerenciador conta as tentativas e avisa o
         * celular; depois de uma recusa, espera credenciais novas. */
        if (!atomic_load(&s_prov_failed)) {
            (void)esp_wifi_connect();
        }
    } else {
        schedule_retry(); /* espera progressiva; senha errada também cai aqui */
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == NETWORK_PROV_EVENT) {
        switch (id) {
        case NETWORK_PROV_START:
            device_state_set(DEVICE_STATE_AWAITING_CONFIG);
            ESP_LOGI(TAG, "modo de configuração BLE aberto como %s", s_ble_name);
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            const wifi_sta_config_t *sta = (const wifi_sta_config_t *)data;
            /* Só o SSID; a senha nunca vai para o log. */
            ESP_LOGI(TAG, "credenciais recebidas para a rede \"%.*s\"", (int)sizeof(sta->ssid),
                     (const char *)sta->ssid);
            atomic_store(&s_prov_failed, false);
            s_last_reason[0] = '\0';
            set_wifi_ok(WIFI_OK_PENDING); /* o componente já gravou; ainda não foi testada */
            device_state_set(DEVICE_STATE_WIFI_CONNECTING);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            const network_prov_wifi_sta_fail_reason_t *reason =
                (const network_prov_wifi_sta_fail_reason_t *)data;
            const bool auth = (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR);
            ESP_LOGW(TAG, "configuração recusada: %s", auth ? "senha errada" : "rede não encontrada");
            device_state_set(auth ? DEVICE_STATE_WIFI_BAD_PASSWORD : DEVICE_STATE_WIFI_NOT_FOUND);
            atomic_store(&s_prov_failed, true);
            /* Permite ao celular mandar outras credenciais na mesma sessão. */
            (void)network_prov_mgr_reset_wifi_sm_state_on_failure();
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "Wi-Fi configurado com sucesso; credenciais gravadas");
            break;
        case NETWORK_PROV_END:
            atomic_store(&s_provisioning, false);
            /* Libera o BLE; daqui em diante a reconexão é por espera progressiva. */
            (void)network_prov_mgr_deinit();
            ESP_LOGI(TAG, "modo de configuração BLE encerrado");
            break;
        default:
            break;
        }
    } else if (base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) {
            ESP_LOGI(TAG, "sessão BLE segura estabelecida");
        } else if (id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH) {
            ESP_LOGW(TAG, "sessão BLE recusada: prova de posse incorreta");
        } else {
            ESP_LOGW(TAG, "sessão BLE recusada (evento %d)", (int)id);
        }
    } else if (base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        ESP_LOGI(TAG, "celular %s", (id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) ? "conectado por BLE"
                                                                              : "desconectou do BLE");
    } else if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_START)) {
        if (!atomic_load(&s_provisioning)) {
            device_state_set(DEVICE_STATE_WIFI_CONNECTING);
            (void)esp_wifi_connect();
        }
    } else if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_DISCONNECTED)) {
        on_disconnected((const wifi_event_sta_disconnected_t *)data);
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        atomic_store(&s_has_ip, true);
        if (wifi_pending()) {
            set_wifi_ok(WIFI_OK_CONFIRMED);
            ESP_LOGI(TAG, "rede confirmada");
        }
        s_retry_us = RETRY_START_US;
        s_last_reason[0] = '\0';
        ESP_LOGI(TAG, "IP obtido");
        timekeeping_start_ntp(CONFIG_PIVO_NTP_SERVER);
        (void)esp_timer_stop(s_internet_timer);
        (void)esp_timer_start_once(s_internet_timer, INTERNET_CHECK_US);
    } else {
        /* demais eventos não interessam */
    }
}

/* ------------------------------------------------------------------ */
/* Partida                                                             */
/* ------------------------------------------------------------------ */

static fw_err_t start_ble_provisioning(void)
{
    if (load_sec2_params() != FW_OK) {
        /* Sem prova de posse não há como abrir o BLE com segurança: o modo de
         * configuração não abre (nunca um serviço sem proteção). */
        ESP_LOGE(TAG, "sem prova de posse (AGRO_BLE_POP em main/provisioning.h): "
                      "modo de configuração BLE não iniciado");
        device_state_set(DEVICE_STATE_NO_FACTORY_DATA);
        (void)network_prov_mgr_deinit();
        return FW_ERR_NOT_FOUND;
    }

    (void)network_prov_scheme_ble_set_service_uuid(s_service_uuid);
    /* Marca antes de iniciar: o WIFI_EVENT_STA_START chega durante o start e
     * não pode disparar uma conexão nem o estado "conectando_wifi". */
    atomic_store(&s_provisioning, true);
    if ((network_prov_mgr_endpoint_create("agro-status") != ESP_OK) ||
        (network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_2, &s_sec2, s_ble_name, NULL) !=
         ESP_OK) ||
        (network_prov_mgr_endpoint_register("agro-status", status_handler, NULL) != ESP_OK)) {
        atomic_store(&s_provisioning, false);
        ESP_LOGE(TAG, "falha ao abrir o modo de configuração BLE");
        return FW_ERR_STATE;
    }
    return FW_OK;
}

fw_err_t wifi_sta_start(void)
{
    uint8_t mac[6];
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    (void)snprintf(s_ble_name, sizeof(s_ble_name), WIFI_BLE_NAME_PREFIX "%02X%02X%02X", mac[3],
                   mac[4], mac[5]);

    const esp_timer_create_args_t retry_args = {
        .callback = retry_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    };
    const esp_timer_create_args_t internet_args = {
        .callback = internet_check_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_inet",
    };
    if ((esp_timer_create(&retry_args, &s_retry_timer) != ESP_OK) ||
        (esp_timer_create(&internet_args, &s_internet_timer) != ESP_OK) ||
        (esp_netif_create_default_wifi_sta() == NULL)) {
        return FW_ERR_NO_MEM;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if ((esp_wifi_init(&init_config) != ESP_OK) ||
        (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL) != ESP_OK) ||
        (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL) != ESP_OK) ||
        (esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL) !=
         ESP_OK) ||
        (esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
                                    event_handler, NULL) != ESP_OK) ||
        (esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, event_handler,
                                    NULL) != ESP_OK)) {
        return FW_ERR_STATE;
    }

    const network_prov_mgr_config_t prov_config = {
        .scheme = network_prov_scheme_ble,
        /* Libera a memória do Bluetooth quando não for mais usado. */
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .network_prov_wifi_conn_cfg = {.wifi_conn_attempts = PROV_CONN_ATTEMPTS},
    };
    bool provisioned = false;
    if ((network_prov_mgr_init(prov_config) != ESP_OK) ||
        (network_prov_mgr_is_wifi_provisioned(&provisioned) != ESP_OK)) {
        return FW_ERR_STATE;
    }

    if (provisioned && wifi_pending()) {
        /* Credencial gravada que nunca conectou (queda de energia durante o
         * teste): descarta e volta ao modo de configuração. */
        ESP_LOGW(TAG, "rede gravada nunca foi confirmada; reabrindo a configuração BLE");
        if (esp_wifi_restore() == ESP_OK) {
            provisioned = false;
        }
    }
    if (!provisioned) {
        return start_ble_provisioning();
    }

    /* Já configurado: nada de BLE; conecta com o que está no NVS. */
    (void)network_prov_mgr_deinit();
    ESP_LOGI(TAG, "Wi-Fi já configurado; conectando");
    if ((esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) || (esp_wifi_start() != ESP_OK)) {
        return FW_ERR_STATE;
    }
    return FW_OK;
}

bool wifi_sta_has_ip(void)
{
    return atomic_load(&s_has_ip);
}

bool wifi_sta_get_rssi(int32_t *rssi)
{
    wifi_ap_record_t ap;
    if ((rssi == NULL) || (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)) {
        return false;
    }
    *rssi = (int32_t)ap.rssi;
    return true;
}

fw_err_t wifi_sta_forget_network(void)
{
    /* Apaga só a configuração do driver Wi-Fi (SSID/senha no NVS). */
    return (esp_wifi_restore() == ESP_OK) ? FW_OK : FW_ERR_IO;
}
