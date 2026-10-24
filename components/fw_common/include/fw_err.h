#pragma once

/**
 * @brief Código de erro único do firmware (camadas portáteis e de placa).
 */
typedef enum {
    FW_OK = 0,
    FW_ERR_INVALID_PARAM,
    FW_ERR_INVALID_DATA,
    FW_ERR_NO_MEM,
    FW_ERR_NOT_FOUND,
    FW_ERR_IO,
    FW_ERR_FULL,
    FW_ERR_TIMEOUT,
    FW_ERR_STATE,
} fw_err_t;
