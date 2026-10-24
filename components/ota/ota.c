#include "ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";

fw_err_t ota_confirm_running_image(bool self_test_ok)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;

    if ((running == NULL) || (esp_ota_get_state_partition(running, &state) != ESP_OK)) {
        return FW_OK; /* gravado por cabo (sem otadata): não há o que confirmar */
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return FW_OK;
    }

    if (!self_test_ok) {
        /* Não confirma: o próximo reset devolve a imagem anterior. O reinício
         * fica por conta de quem chamou, depois de publicar `ota_failed`. */
        ESP_LOGE(TAG, "autoteste falhou; imagem nova não confirmada");
        return FW_ERR_STATE;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        return FW_ERR_IO;
    }
    ESP_LOGI(TAG, "imagem nova confirmada");
    return FW_OK;
}
