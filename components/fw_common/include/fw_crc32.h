#pragma once

#include <stddef.h>
#include <stdint.h>

#define FW_CRC32_INIT (0U)

/**
 * @brief CRC-32 (IEEE 802.3, refletido), encadeável.
 *
 * @param crc  FW_CRC32_INIT na primeira chamada, ou o retorno da chamada anterior
 * @param data Bytes a somar (pode ser NULL se len == 0)
 * @param len  Quantidade de bytes
 *
 * @return CRC acumulado
 */
uint32_t fw_crc32(uint32_t crc, const uint8_t *data, size_t len);
