#include "fw_crc32.h"

#define CRC32_POLY_REFLECTED (0xEDB88320U)
#define CRC32_XOR_OUT        (0xFFFFFFFFU)
#define BITS_PER_BYTE        (8U)

uint32_t fw_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return crc;
    }

    /* Sem tabela: o journal grava um registro a cada dezenas de segundos,
     * então 256 palavras de flash não compensam. */
    crc ^= CRC32_XOR_OUT;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < BITS_PER_BYTE; bit++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1) ^ (CRC32_POLY_REFLECTED & mask);
        }
    }
    return crc ^ CRC32_XOR_OUT;
}
