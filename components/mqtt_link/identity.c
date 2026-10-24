#include "identity.h"

#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

/* Lê um blob e acrescenta NUL (o esp-tls exige PEM terminado em NUL). */
static fw_err_t read_pem(nvs_handle_t handle, const char *key, const char **out)
{
    size_t len = 0;
    if ((nvs_get_blob(handle, key, NULL, &len) != ESP_OK) || (len == 0U)) {
        return FW_ERR_NOT_FOUND;
    }

    /* Única alocação dinâmica do módulo: acontece uma vez, na partida, e o
     * tamanho depende do certificado gravado. */
    char *buffer = malloc(len + 1U);
    if (buffer == NULL) {
        return FW_ERR_NO_MEM;
    }
    if (nvs_get_blob(handle, key, buffer, &len) != ESP_OK) {
        free(buffer);
        return FW_ERR_IO;
    }
    buffer[len] = '\0';
    *out = buffer;
    return FW_OK;
}

/* O device_id entra no tópico: nada de '/', '+', '#' ou espaço. */
static bool device_id_ok(const char *id)
{
    if (id[0] == '\0') {
        return false;
    }
    for (const char *p = id; *p != '\0'; p++) {
        const bool ok = ((*p >= 'a') && (*p <= 'z')) || ((*p >= 'A') && (*p <= 'Z')) ||
                        ((*p >= '0') && (*p <= '9')) || (*p == '-') || (*p == '_') || (*p == '.');
        if (!ok) {
            return false;
        }
    }
    return true;
}

fw_err_t identity_load(identity_t *out)
{
    if (out == NULL) {
        return FW_ERR_INVALID_PARAM;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle = 0;
    if (nvs_open(IDENTITY_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return FW_ERR_NOT_FOUND; /* ainda sem enroll */
    }

    fw_err_t err = FW_OK;
    size_t id_len = sizeof(out->device_id);
    size_t host_len = sizeof(out->mqtt_host);
    size_t cert_len = 0;

    /* O certificado é a última coisa que o enroll grava: sem ele, não há
     * identidade (e nada é alocado). */
    if ((nvs_get_blob(handle, IDENTITY_KEY_CERT, NULL, &cert_len) != ESP_OK) ||
        (nvs_get_str(handle, IDENTITY_KEY_DEVICE_ID, out->device_id, &id_len) != ESP_OK) ||
        (nvs_get_str(handle, IDENTITY_KEY_MQTT_HOST, out->mqtt_host, &host_len) != ESP_OK) ||
        (nvs_get_u16(handle, IDENTITY_KEY_MQTT_PORT, &out->mqtt_port) != ESP_OK)) {
        err = FW_ERR_NOT_FOUND;
    } else if (!device_id_ok(out->device_id)) {
        err = FW_ERR_INVALID_DATA;
    } else {
        err = read_pem(handle, IDENTITY_KEY_CERT, &out->cert_pem);
        if (err == FW_OK) {
            err = read_pem(handle, IDENTITY_KEY_KEY, &out->key_pem);
        }
        if (err == FW_OK) {
            err = read_pem(handle, IDENTITY_KEY_CA, &out->ca_pem);
        }
    }
    nvs_close(handle);

    if (err != FW_OK) {
        free((void *)out->cert_pem);
        free((void *)out->key_pem);
        free((void *)out->ca_pem);
        out->cert_pem = NULL;
        out->key_pem = NULL;
        out->ca_pem = NULL;
    }
    return err;
}

void identity_release(identity_t *identity)
{
    if (identity == NULL) {
        return;
    }
    free((void *)identity->cert_pem);
    free((void *)identity->key_pem);
    free((void *)identity->ca_pem);
    identity->cert_pem = NULL;
    identity->key_pem = NULL;
    identity->ca_pem = NULL;
}
