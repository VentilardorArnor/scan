#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fw_err.h"

/*
 * Journal durável de telemetria (ADR-002).
 *
 * Segmentos append-only em um diretório (LittleFS na placa, pasta comum no
 * host). Um envelope só sai daqui com ACK de aplicação (journal_ack).
 *
 * Registro: magic(4) | versão(1) | tamanho(2) | boot_id(16) | seq(4) |
 *           payload | CRC32(4), inteiros em little-endian.
 * O CRC cobre de `versão` até o fim do payload.
 */

#define JOURNAL_BOOT_ID_LEN     (16U)   /* 8 bytes aleatórios em hex */
#define JOURNAL_PAYLOAD_MAX     (2048U)
#define JOURNAL_DIR_MAX         (48U)
#define JOURNAL_ACKED_MAX       (32U)   /* ACKs fora de ordem ainda não consolidados */
#define JOURNAL_ACK_SEARCH_MAX  (64U)   /* até onde procurar o registro de um ACK */

typedef struct {
    char boot_id[JOURNAL_BOOT_ID_LEN + 1U];
    uint32_t seq;
} journal_key_t;

typedef struct {
    journal_key_t key;
    uint16_t len; /* bytes do payload */
} journal_record_t;

typedef struct {
    char dir[JOURNAL_DIR_MAX];
    uint32_t max_bytes;     /* teto do journal inteiro */
    uint32_t seg_max_bytes; /* teto de um segmento */

    uint32_t first_seg;  /* segmento mais antigo ainda em disco */
    uint32_t write_seg;  /* segmento em escrita (criado no primeiro append) */
    uint32_t write_off;
    uint32_t cur_seg;    /* cursor: primeiro registro ainda sem ACK */
    uint32_t cur_off;
    uint32_t total_bytes;

    uint32_t pending;        /* registros sem ACK */
    uint32_t dropped;        /* registros perdidos por overflow (persistido) */
    uint32_t corrupt_found;  /* trechos corrompidos vistos na última abertura */

    journal_key_t acked[JOURNAL_ACKED_MAX];
    uint8_t acked_count;
} journal_t;

typedef struct {
    uint32_t seg;
    uint32_t off;
    FILE *file;
    uint32_t garbage_runs; /* trechos inválidos pulados nesta varredura */
    bool in_garbage;
} journal_iter_t;

/**
 * @brief Abre o journal: lê o cursor, varre os segmentos, descarta registros
 *        com CRC inválido e conta os pendentes.
 *
 * Sempre começa um segmento novo para as próximas gravações, de modo que um
 * registro cortado por queda de energia fica isolado no fim do anterior.
 *
 * @param dir           Diretório dos segmentos (já montado)
 * @param max_bytes     Tamanho máximo total
 * @param seg_max_bytes Tamanho máximo de um segmento (menor que max_bytes)
 *
 * @return FW_OK, FW_ERR_INVALID_PARAM ou FW_ERR_IO
 */
fw_err_t journal_open(journal_t *j, const char *dir, uint32_t max_bytes, uint32_t seg_max_bytes);

/**
 * @brief Grava um envelope de forma durável (flush + fsync).
 *
 * Se não couber, apaga o segmento mais antigo e soma os registros perdidos
 * em `dropped`.
 *
 * @param dropped_now Se não NULL, recebe quantos registros foram perdidos
 *                    nesta chamada (> 0 = publicar journal_full)
 */
fw_err_t journal_append(journal_t *j, const journal_key_t *key, const char *payload, size_t len,
                        uint32_t *dropped_now);

/**
 * @brief Libera um registro (ACK stored/duplicate, ou descarte por `invalid`).
 *
 * Aceita ACK fora de ordem; o cursor é regravado de forma atômica
 * (arquivo temporário + rename).
 *
 * @return FW_OK; FW_ERR_NOT_FOUND se a chave não está entre os pendentes
 */
fw_err_t journal_ack(journal_t *j, const journal_key_t *key);

/** @brief Começa uma varredura dos pendentes, do mais antigo ao mais novo. */
void journal_iter_begin(const journal_t *j, journal_iter_t *it);

/**
 * @brief Próximo registro pendente (pula os que já têm ACK).
 *
 * @param payload Buffer para o payload (recebe NUL no fim)
 * @param cap     Tamanho do buffer; precisa ser maior que o payload
 *
 * @return FW_OK; FW_ERR_NOT_FOUND no fim; FW_ERR_NO_MEM se não coube
 */
fw_err_t journal_iter_next(const journal_t *j, journal_iter_t *it, journal_record_t *rec,
                           char *payload, size_t cap);

/** @brief Encerra a varredura (fecha o arquivo aberto). */
void journal_iter_end(journal_iter_t *it);
