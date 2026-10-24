#pragma once

#include <stddef.h>
#include <stdint.h>

#include "unity.h"

void run_nmea_tests(void);
void run_contract_tests(void);
void run_journal_tests(void);
void run_telemetry_tx_tests(void);
void run_control_tests(void);
void run_prov_line_tests(void);
void run_lossy_link_tests(void);

/* Diretório temporário novo para um teste de journal. */
void test_make_tmp_dir(char *out, size_t cap);
/* Apaga o diretório e tudo dentro dele. */
void test_remove_dir(const char *dir);
/* Caminho do único (ou do primeiro) segmento do journal em `dir`. */
void test_first_segment_path(const char *dir, char *out, size_t cap);
/* Tamanho de um arquivo, em bytes. */
long test_file_size(const char *path);
