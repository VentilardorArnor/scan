#pragma once

#include <stdbool.h>

#include "fw_err.h"

/*
 * OTA — só a estrutura: slots A/B (ota_0/ota_1) com rollback do bootloader.
 * O comando `ota.start` (baixar e gravar a imagem) fica para depois.
 */

/**
 * @brief Confirma a imagem em execução depois do autoteste.
 *
 * Com CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, uma imagem recém-gravada sobe
 * como "pendente de verificação". Se este firmware reiniciar antes de chamar
 * esta função com `self_test_ok = true`, o bootloader volta para a anterior.
 *
 * @param self_test_ok Resultado do autoteste de partida do aplicativo
 *
 * @return FW_OK se a imagem ficou (ou já estava) confirmada; FW_ERR_STATE se
 *         o autoteste falhou e a imagem NÃO foi confirmada
 */
fw_err_t ota_confirm_running_image(bool self_test_ok);
