#include "enroll.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "identity.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "nvs.h"
#include "prov_line.h"
#include "psa/crypto.h"

#define EC_KEY_BITS        (256U)
#define SHA256_LEN         (32U)
#define KEY_PEM_MAX        (512U)
#define CSR_PEM_MAX        (1024U)
#define SUBJECT_MAX        (80U)
#define URL_MAX            (300U)
#define REQUEST_MAX        (1536U)
#define RESPONSE_MAX       (8192U)
#define HTTP_TIMEOUT_MS    (15000)
#define ENROLL_PATH        "/v1/devices/enroll"
#define PEM_CERT_BEGIN     "-----BEGIN CERTIFICATE-----"
#define LOG_BODY_MAX       (160)

#define HTTP_OK                (200)
#define HTTP_NOT_FOUND         (404)
#define HTTP_GONE              (410)
#define HTTP_UNPROCESSABLE     (422)
#define HTTP_TOO_MANY_REQUESTS (429)
#define HTTP_SERVER_ERROR_MIN  (500)

static const char *TAG = "enroll";

/* Só a tarefa de provisionamento chama este módulo, uma vez por boot. */
static char s_request[REQUEST_MAX];
static char s_response[RESPONSE_MAX + 1U];
static unsigned char s_pem[CSR_PEM_MAX];

/* ------------------------------------------------------------------ */
/* NVS                                                                 */
/* ------------------------------------------------------------------ */

static bool nvs_has_blob(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    return (nvs_get_blob(h, key, NULL, &len) == ESP_OK) && (len > 0U);
}

/* Linha diferente da que gerou a identidade: começa do zero. */
static fw_err_t check_line(nvs_handle_t h, const uint8_t sha[SHA256_LEN])
{
    uint8_t stored[SHA256_LEN];
    size_t len = sizeof(stored);
    const esp_err_t err = nvs_get_blob(h, IDENTITY_KEY_LINE_SHA, stored, &len);

    if ((err == ESP_OK) && (len == SHA256_LEN) && (memcmp(stored, sha, SHA256_LEN) == 0)) {
        return FW_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "linha de provisionamento nova: identidade anterior descartada");
    }
    if ((nvs_erase_all(h) != ESP_OK) ||
        (nvs_set_blob(h, IDENTITY_KEY_LINE_SHA, sha, SHA256_LEN) != ESP_OK) ||
        (nvs_commit(h) != ESP_OK)) {
        return FW_ERR_IO;
    }
    return FW_OK;
}

/* ------------------------------------------------------------------ */
/* Chave e CSR                                                         */
/* ------------------------------------------------------------------ */

/* Gera a chave P-256 com PSA e a grava (PEM) no NVS antes de qualquer
 * requisição. Se já existe, só a carrega. */
static fw_err_t load_or_create_key(nvs_handle_t h, mbedtls_pk_context *pk)
{
    size_t len = sizeof(s_pem);

    if (nvs_get_blob(h, IDENTITY_KEY_KEY, s_pem, &len) == ESP_OK) {
        if ((len >= sizeof(s_pem)) ||
            (mbedtls_pk_parse_key(pk, s_pem, len, NULL, 0) != 0)) { /* PEM gravado com NUL */
            return FW_ERR_INVALID_DATA;
        }
        ESP_LOGI(TAG, "reutilizando a chave gerada antes (reenvio do enroll)");
        return FW_OK;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, EC_KEY_BITS);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_generate_key(&attr, &key_id) != PSA_SUCCESS) {
        return FW_ERR_STATE;
    }
    const int32_t copied = mbedtls_pk_copy_from_psa(key_id, pk);
    (void)psa_destroy_key(key_id);
    psa_reset_key_attributes(&attr);
    if (copied != 0) {
        return FW_ERR_STATE;
    }

    memset(s_pem, 0, sizeof(s_pem));
    if (mbedtls_pk_write_key_pem(pk, s_pem, KEY_PEM_MAX) != 0) {
        return FW_ERR_STATE;
    }
    const size_t pem_len = strlen((const char *)s_pem) + 1U; /* com NUL */
    const bool stored = (nvs_set_blob(h, IDENTITY_KEY_KEY, s_pem, pem_len) == ESP_OK) &&
                        (nvs_commit(h) == ESP_OK);
    memset(s_pem, 0, sizeof(s_pem)); /* não deixa a chave em RAM à toa */
    if (!stored) {
        return FW_ERR_IO;
    }
    ESP_LOGI(TAG, "chave EC P-256 gerada e gravada");
    return FW_OK;
}

static fw_err_t build_csr(mbedtls_pk_context *pk, const char *cn)
{
    char subject[SUBJECT_MAX];
    mbedtls_x509write_csr csr;

    if (snprintf(subject, sizeof(subject), "CN=%s", cn) >= (int32_t)sizeof(subject)) {
        return FW_ERR_INVALID_PARAM;
    }
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, pk);

    fw_err_t err = FW_OK;
    memset(s_pem, 0, sizeof(s_pem));
    if ((mbedtls_x509write_csr_set_subject_name(&csr, subject) != 0) ||
        (mbedtls_x509write_csr_pem(&csr, s_pem, sizeof(s_pem)) != 0)) {
        err = FW_ERR_STATE;
    }
    mbedtls_x509write_csr_free(&csr);
    return err;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static fw_err_t post_enroll(const prov_line_t *line, int32_t *status)
{
    char url[URL_MAX];
    *status = 0;

    cJSON *body = cJSON_CreateObject();
    if ((body == NULL) || (cJSON_AddStringToObject(body, "cn", line->cn) == NULL) ||
        (cJSON_AddStringToObject(body, "token", line->token) == NULL) ||
        (cJSON_AddStringToObject(body, "csr", (const char *)s_pem) == NULL) ||
        !cJSON_PrintPreallocated(body, s_request, sizeof(s_request), false)) {
        cJSON_Delete(body);
        return FW_ERR_NO_MEM;
    }
    cJSON_Delete(body);
    (void)snprintf(url, sizeof(url), "https://%s%s", line->host, ENROLL_PATH);

    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        /* TLS comum: o servidor da API tem certificado público. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return FW_ERR_NO_MEM;
    }

    fw_err_t err = FW_ERR_IO;
    const int32_t body_len = (int32_t)strlen(s_request);
    s_response[0] = '\0';

    if ((esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK) &&
        (esp_http_client_open(client, body_len) == ESP_OK) &&
        (esp_http_client_write(client, s_request, body_len) == body_len) &&
        (esp_http_client_fetch_headers(client) >= 0)) {
        *status = esp_http_client_get_status_code(client);
        const int32_t got = esp_http_client_read_response(client, s_response, RESPONSE_MAX);
        if (got >= 0) {
            s_response[got] = '\0';
            err = FW_OK;
        }
    }
    memset(s_request, 0, sizeof(s_request)); /* o pedido tinha o token */
    (void)esp_http_client_cleanup(client);
    return err;
}

/* Último certificado do ca_chain = raiz que valida o broker MQTT. */
static const char *last_certificate(const char *chain)
{
    const char *last = NULL;
    const char *p = strstr(chain, PEM_CERT_BEGIN);
    while (p != NULL) {
        last = p;
        p = strstr(p + 1, PEM_CERT_BEGIN);
    }
    return last;
}

static fw_err_t store_response(nvs_handle_t h, const char *cn, bool *reissued)
{
    cJSON *root = cJSON_Parse(s_response);
    const cJSON *cert = cJSON_GetObjectItemCaseSensitive(root, "certificate");
    const cJSON *chain = cJSON_GetObjectItemCaseSensitive(root, "ca_chain");
    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(mqtt, "host");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(mqtt, "port");

    fw_err_t err = FW_ERR_INVALID_DATA;
    const char *root_ca = cJSON_IsString(chain) ? last_certificate(chain->valuestring) : NULL;

    if (cJSON_IsString(cert) && (strstr(cert->valuestring, PEM_CERT_BEGIN) != NULL) &&
        (root_ca != NULL) && cJSON_IsString(host) && (host->valuestring[0] != '\0') &&
        (strlen(host->valuestring) <= IDENTITY_HOST_MAX) && cJSON_IsNumber(port) &&
        (port->valuedouble >= 1.0) && (port->valuedouble <= 65535.0)) {
        *reissued = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reissued"));

        /* Certificado por último: é ele que identity_load() usa para saber
         * que o enroll terminou. */
        const bool ok =
            (nvs_set_str(h, IDENTITY_KEY_DEVICE_ID, cn) == ESP_OK) &&
            (nvs_set_str(h, IDENTITY_KEY_MQTT_HOST, host->valuestring) == ESP_OK) &&
            (nvs_set_u16(h, IDENTITY_KEY_MQTT_PORT, (uint16_t)port->valuedouble) == ESP_OK) &&
            (nvs_set_blob(h, IDENTITY_KEY_CA, root_ca, strlen(root_ca) + 1U) == ESP_OK) &&
            (nvs_set_blob(h, IDENTITY_KEY_CERT, cert->valuestring,
                          strlen(cert->valuestring) + 1U) == ESP_OK) &&
            (nvs_commit(h) == ESP_OK);
        err = ok ? FW_OK : FW_ERR_IO;
        if (ok) {
            ESP_LOGI(TAG, "certificado gravado; broker %s:%u%s", host->valuestring,
                     (unsigned)port->valuedouble, *reissued ? " (reemitido)" : "");
        }
    }
    cJSON_Delete(root);
    return err;
}

/* ------------------------------------------------------------------ */

enroll_result_t enroll_ensure(const char *line_text, int32_t *http_status)
{
    prov_line_t line;
    uint8_t sha[SHA256_LEN];
    size_t sha_len = 0;
    int32_t status = 0;

    if (http_status != NULL) {
        *http_status = 0;
    }
    if ((line_text == NULL) || (prov_line_parse(line_text, &line) != FW_OK)) {
        ESP_LOGE(TAG, "linha de provisionamento ausente ou malformada");
        return ENROLL_FIRMWARE_ERROR;
    }
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)line_text, strlen(line_text), sha,
                         sizeof(sha), &sha_len) != PSA_SUCCESS) {
        return ENROLL_RETRY;
    }

    nvs_handle_t h = 0;
    if (nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return ENROLL_RETRY;
    }

    enroll_result_t result = ENROLL_RETRY;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    if (check_line(h, sha) != FW_OK) {
        result = ENROLL_RETRY;
    } else if (nvs_has_blob(h, IDENTITY_KEY_CERT)) {
        result = ENROLL_DONE; /* já feito em outro boot */
    } else if ((load_or_create_key(h, &pk) != FW_OK) || (build_csr(&pk, line.cn) != FW_OK)) {
        ESP_LOGE(TAG, "falha ao preparar a chave ou a CSR");
        result = ENROLL_FIRMWARE_ERROR;
    } else {
        ESP_LOGI(TAG, "enroll de %s em %s", line.cn, line.host); /* nunca o token */
        if (post_enroll(&line, &status) != FW_OK) {
            ESP_LOGW(TAG, "sem resposta do servidor de enroll");
            result = ENROLL_RETRY;
        } else if (status == HTTP_OK) {
            bool reissued = false;
            result = (store_response(h, line.cn, &reissued) == FW_OK) ? ENROLL_DONE
                                                                       : ENROLL_FIRMWARE_ERROR;
        } else {
            ESP_LOGW(TAG, "enroll recusado: HTTP %d %.*s", (int)status, LOG_BODY_MAX, s_response);
            if ((status == HTTP_NOT_FOUND) || (status == HTTP_GONE)) {
                result = ENROLL_NEED_NEW_LINE;
            } else if ((status == HTTP_TOO_MANY_REQUESTS) || (status >= HTTP_SERVER_ERROR_MIN)) {
                result = ENROLL_RETRY;
            } else {
                result = ENROLL_FIRMWARE_ERROR; /* 422 e demais 4xx */
            }
        }
    }

    memset(&line, 0, sizeof(line)); /* apaga o token da pilha */
    memset(s_pem, 0, sizeof(s_pem));
    mbedtls_pk_free(&pk);
    nvs_close(h);
    if (http_status != NULL) {
        *http_status = status;
    }
    return result;
}
