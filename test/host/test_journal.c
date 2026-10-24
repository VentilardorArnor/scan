#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "journal.h"
#include "test_support.h"

#define SEG_MAX   (4096U)
#define TOTAL_MAX (16384U)
#define BOOT_A    "aaaaaaaaaaaaaaaa"
#define BOOT_B    "bbbbbbbbbbbbbbbb"

/* ------------------------------------------------------------------ */
/* Suporte compartilhado com os outros arquivos de teste               */
/* ------------------------------------------------------------------ */

void test_make_tmp_dir(char *out, size_t cap)
{
    (void)snprintf(out, cap, "/tmp/pivo-journal-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(out));
}

void test_remove_dir(const char *dir)
{
    char path[256];
    DIR *handle = opendir(dir);
    if (handle != NULL) {
        const struct dirent *entry = readdir(handle);
        while (entry != NULL) {
            if ((strcmp(entry->d_name, ".") != 0) && (strcmp(entry->d_name, "..") != 0)) {
                (void)snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
                (void)remove(path);
            }
            entry = readdir(handle);
        }
        (void)closedir(handle);
    }
    (void)rmdir(dir);
}

void test_first_segment_path(const char *dir, char *out, size_t cap)
{
    char best[64] = "";
    DIR *handle = opendir(dir);
    TEST_ASSERT_NOT_NULL(handle);
    const struct dirent *entry = readdir(handle);
    while (entry != NULL) {
        if ((strncmp(entry->d_name, "seg-", 4) == 0) &&
            ((best[0] == '\0') || (strcmp(entry->d_name, best) < 0))) {
            (void)snprintf(best, sizeof(best), "%s", entry->d_name);
        }
        entry = readdir(handle);
    }
    (void)closedir(handle);
    TEST_ASSERT_TRUE_MESSAGE(best[0] != '\0', "nenhum segmento em disco");
    (void)snprintf(out, cap, "%s/%s", dir, best);
}

long test_file_size(const char *path)
{
    struct stat st;
    TEST_ASSERT_EQUAL(0, stat(path, &st));
    return (long)st.st_size;
}

/* ------------------------------------------------------------------ */

static journal_key_t key_of(const char *boot_id, uint32_t seq)
{
    journal_key_t key;
    memset(&key, 0, sizeof(key));
    (void)snprintf(key.boot_id, sizeof(key.boot_id), "%s", boot_id);
    key.seq = seq;
    return key;
}

static void append_seq(journal_t *j, const char *boot_id, uint32_t seq)
{
    char payload[64];
    const journal_key_t key = key_of(boot_id, seq);
    const int32_t len = snprintf(payload, sizeof(payload), "{\"seq\":%u,\"pad\":\"xxxxxxxx\"}",
                                 (unsigned)seq);
    TEST_ASSERT_EQUAL(FW_OK, journal_append(j, &key, payload, (size_t)len, NULL));
}

/* Lê os pendentes em ordem e devolve quantos são; `seqs` recebe os seq. */
static uint32_t read_pending(const journal_t *j, uint32_t *seqs, uint32_t cap)
{
    char payload[JOURNAL_PAYLOAD_MAX + 1U];
    journal_iter_t it;
    journal_record_t rec;
    uint32_t count = 0;

    journal_iter_begin(j, &it);
    while (journal_iter_next(j, &it, &rec, payload, sizeof(payload)) == FW_OK) {
        if (count < cap) {
            seqs[count] = rec.key.seq;
        }
        TEST_ASSERT_EQUAL_UINT16(strlen(payload), rec.len);
        count++;
    }
    journal_iter_end(&it);
    return count;
}

static void test_append_read_and_reopen(void)
{
    char dir[64];
    journal_t j;
    uint32_t seqs[8];

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(0, j.pending);

    for (uint32_t i = 0; i < 4U; i++) {
        append_seq(&j, BOOT_A, i);
    }
    TEST_ASSERT_EQUAL_UINT32(4, j.pending);
    TEST_ASSERT_EQUAL_UINT32(4, read_pending(&j, seqs, 8));
    for (uint32_t i = 0; i < 4U; i++) {
        TEST_ASSERT_EQUAL_UINT32(i, seqs[i]);
    }

    /* "Reboot": os envelopes continuam lá, e o novo boot grava depois deles. */
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(4, j.pending);
    TEST_ASSERT_EQUAL_UINT32(0, j.corrupt_found);
    append_seq(&j, BOOT_B, 0);
    TEST_ASSERT_EQUAL_UINT32(5, read_pending(&j, seqs, 8));
    TEST_ASSERT_EQUAL_UINT32(3, seqs[3]);
    TEST_ASSERT_EQUAL_UINT32(0, seqs[4]);

    test_remove_dir(dir);
}

static void test_ack_out_of_order(void)
{
    char dir[64];
    journal_t j;
    uint32_t seqs[8];
    journal_key_t key;

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    for (uint32_t i = 0; i < 5U; i++) {
        append_seq(&j, BOOT_A, i);
    }

    /* ACK do meio primeiro: sai dos pendentes, mas o cursor não anda. */
    key = key_of(BOOT_A, 2);
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    TEST_ASSERT_EQUAL_UINT32(4, j.pending);
    TEST_ASSERT_EQUAL_UINT32(0, j.cur_off);
    TEST_ASSERT_EQUAL_UINT32(4, read_pending(&j, seqs, 8));
    TEST_ASSERT_EQUAL_UINT32(0, seqs[0]);
    TEST_ASSERT_EQUAL_UINT32(1, seqs[1]);
    TEST_ASSERT_EQUAL_UINT32(3, seqs[2]);

    /* ACK repetido é inofensivo. */
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    TEST_ASSERT_EQUAL_UINT32(4, j.pending);

    key = key_of(BOOT_A, 0);
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    const uint32_t off_after_first = j.cur_off;
    TEST_ASSERT_TRUE(off_after_first > 0U);

    /* Com 0, 1 e 2 confirmados o cursor salta os três de uma vez. */
    key = key_of(BOOT_A, 1);
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    TEST_ASSERT_EQUAL_UINT32(2, j.pending);
    TEST_ASSERT_EQUAL_UINT8(0, j.acked_count);
    TEST_ASSERT_EQUAL_UINT32(off_after_first * 3U, j.cur_off);

    /* O que foi confirmado não volta depois de um reboot. */
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(2, j.pending);
    TEST_ASSERT_EQUAL_UINT32(2, read_pending(&j, seqs, 8));
    TEST_ASSERT_EQUAL_UINT32(3, seqs[0]);
    TEST_ASSERT_EQUAL_UINT32(4, seqs[1]);

    /* ACK fora de ordem também sobrevive ao reboot. */
    key = key_of(BOOT_A, 4);
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(1, j.pending);
    TEST_ASSERT_EQUAL_UINT32(1, read_pending(&j, seqs, 8));
    TEST_ASSERT_EQUAL_UINT32(3, seqs[0]);

    /* Chave que não existe (outro boot) não entra na lista. */
    key = key_of(BOOT_B, 3);
    TEST_ASSERT_EQUAL(FW_ERR_NOT_FOUND, journal_ack(&j, &key));

    /* Último ACK: journal vazio e segmentos antigos apagados. */
    key = key_of(BOOT_A, 3);
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    TEST_ASSERT_EQUAL_UINT32(0, j.pending);
    TEST_ASSERT_EQUAL_UINT32(0, j.total_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, read_pending(&j, seqs, 8));

    test_remove_dir(dir);
}

static void test_corrupt_crc_is_skipped(void)
{
    char dir[64];
    char path[128];
    journal_t j;
    uint32_t seqs[8];

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    for (uint32_t i = 0; i < 5U; i++) {
        append_seq(&j, BOOT_A, i);
    }

    /* Estraga um byte do payload do terceiro registro (todos têm o mesmo tamanho). */
    test_first_segment_path(dir, path, sizeof(path));
    const long rec_size = test_file_size(path) / 5;
    FILE *file = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL(0, fseek(file, (2 * rec_size) + 30, SEEK_SET));
    TEST_ASSERT_EQUAL(1, fwrite("#", 1, 1, file));
    (void)fclose(file);

    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(1, j.corrupt_found);
    TEST_ASSERT_EQUAL_UINT32(4, j.pending);
    TEST_ASSERT_EQUAL_UINT32(4, read_pending(&j, seqs, 8));
    TEST_ASSERT_EQUAL_UINT32(0, seqs[0]);
    TEST_ASSERT_EQUAL_UINT32(1, seqs[1]);
    TEST_ASSERT_EQUAL_UINT32(3, seqs[2]);
    TEST_ASSERT_EQUAL_UINT32(4, seqs[3]);

    /* Os vizinhos do registro corrompido ainda podem ser confirmados. */
    for (uint32_t i = 0; i < 5U; i++) {
        if (i != 2U) {
            const journal_key_t key = key_of(BOOT_A, i);
            TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, j.pending);

    test_remove_dir(dir);
}

static void test_reboot_in_the_middle_of_a_write(void)
{
    char dir[64];
    char path[128];
    journal_t j;
    uint32_t seqs[8];

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    for (uint32_t i = 0; i < 3U; i++) {
        append_seq(&j, BOOT_A, i);
    }

    /* Simula a energia caindo com o último registro pela metade. */
    test_first_segment_path(dir, path, sizeof(path));
    const long size = test_file_size(path);
    TEST_ASSERT_EQUAL(0, truncate(path, size - 10));

    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(1, j.corrupt_found);
    TEST_ASSERT_EQUAL_UINT32(2, j.pending);

    /* O boot seguinte grava normalmente e nada se perde além do cortado. */
    append_seq(&j, BOOT_B, 0);
    append_seq(&j, BOOT_B, 1);
    TEST_ASSERT_EQUAL_UINT32(4, read_pending(&j, seqs, 8));
    TEST_ASSERT_EQUAL_UINT32(0, seqs[0]);
    TEST_ASSERT_EQUAL_UINT32(1, seqs[1]);
    TEST_ASSERT_EQUAL_UINT32(0, seqs[2]);
    TEST_ASSERT_EQUAL_UINT32(1, seqs[3]);

    /* Só o cabeçalho pela metade, também. */
    test_first_segment_path(dir, path, sizeof(path));
    TEST_ASSERT_EQUAL(0, truncate(path, (size / 3) * 2 + 5));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(1, j.corrupt_found);
    TEST_ASSERT_EQUAL_UINT32(4, j.pending);

    test_remove_dir(dir);
}

static void test_corrupt_cursor_resends_everything(void)
{
    char dir[64];
    char path[128];
    journal_t j;
    journal_key_t key;

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    for (uint32_t i = 0; i < 4U; i++) {
        append_seq(&j, BOOT_A, i);
    }
    key = key_of(BOOT_A, 0);
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));

    (void)snprintf(path, sizeof(path), "%s/cursor.bin", dir);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL(5, fwrite("lixo!", 1, 5, file));
    (void)fclose(file);

    /* Sem cursor confiável volta ao início: reenviar é seguro (duplicate). */
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(4, j.pending);

    test_remove_dir(dir);
}

static void test_overflow_drops_oldest_segment(void)
{
    char dir[64];
    char payload[512];
    journal_t j;
    uint32_t seqs[64];
    uint32_t dropped_total = 0;
    uint32_t dropped_now = 0;
    const uint32_t appended = 40;

    memset(payload, 'x', sizeof(payload));

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));

    for (uint32_t i = 0; i < appended; i++) {
        const journal_key_t key = key_of(BOOT_A, i);
        TEST_ASSERT_EQUAL(FW_OK, journal_append(&j, &key, payload, sizeof(payload), &dropped_now));
        dropped_total += dropped_now;
        TEST_ASSERT_TRUE(j.total_bytes <= TOTAL_MAX);
    }

    TEST_ASSERT_TRUE(dropped_total > 0U);
    TEST_ASSERT_EQUAL_UINT32(dropped_total, j.dropped);
    TEST_ASSERT_EQUAL_UINT32(appended, j.pending + j.dropped);

    /* Sobram os mais novos, em ordem, sem buraco. */
    const uint32_t count = read_pending(&j, seqs, 64);
    TEST_ASSERT_EQUAL_UINT32(j.pending, count);
    TEST_ASSERT_EQUAL_UINT32(dropped_total, seqs[0]);
    TEST_ASSERT_EQUAL_UINT32(appended - 1U, seqs[count - 1U]);

    /* O contador de perdas é persistido. */
    const uint32_t pending_before = j.pending;
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(dropped_total, j.dropped);
    TEST_ASSERT_EQUAL_UINT32(pending_before, j.pending);

    test_remove_dir(dir);
}

static void test_invalid_parameters(void)
{
    char dir[64];
    journal_t j;
    const journal_key_t key = key_of(BOOT_A, 0);

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_PARAM, journal_open(NULL, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_PARAM, journal_open(&j, dir, SEG_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL(FW_ERR_IO, journal_open(&j, "/nao/existe/aqui", TOTAL_MAX, SEG_MAX));

    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_PARAM, journal_append(&j, &key, "", 0, NULL));
    TEST_ASSERT_EQUAL(FW_ERR_INVALID_PARAM, journal_append(&j, &key, NULL, 4, NULL));
    test_remove_dir(dir);
}


static void test_head_ack_with_full_out_of_order_list(void)
{
    char dir[64];
    journal_t j;
    journal_key_t key;
    uint32_t seqs[8];

    /* Reproduz o travamento visto na placa: o ACK do lote mais antigo se
     * perde, os seguintes chegam fora de ordem até encher a lista, e o
     * reenvio do mais antigo precisa destravar tudo em vez de ser recusado. */
    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX * 4U, SEG_MAX));
    for (uint32_t i = 0; i < (JOURNAL_ACKED_MAX + 4U); i++) {
        append_seq(&j, BOOT_A, i);
    }
    for (uint32_t i = 1; i <= JOURNAL_ACKED_MAX; i++) {
        key = key_of(BOOT_A, i);
        TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    }
    TEST_ASSERT_EQUAL_UINT8(JOURNAL_ACKED_MAX, j.acked_count);

    /* Um ACK fora de ordem a mais continua sem lugar... */
    key = key_of(BOOT_A, JOURNAL_ACKED_MAX + 1U);
    TEST_ASSERT_EQUAL(FW_ERR_FULL, journal_ack(&j, &key));

    /* ...mas o do lote mais antigo é aceito e consome tudo o que já tinha ACK. */
    key = key_of(BOOT_A, 0);
    TEST_ASSERT_EQUAL(FW_OK, journal_ack(&j, &key));
    TEST_ASSERT_EQUAL_UINT8(0, j.acked_count);
    TEST_ASSERT_EQUAL_UINT32(3, j.pending);
    TEST_ASSERT_EQUAL_UINT32(3, read_pending(&j, seqs, 8));
    TEST_ASSERT_EQUAL_UINT32(JOURNAL_ACKED_MAX + 1U, seqs[0]);

    /* E o estado sobrevive a um reboot. */
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&j, dir, TOTAL_MAX * 4U, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(3, j.pending);
    test_remove_dir(dir);
}

void run_journal_tests(void)
{
    RUN_TEST(test_append_read_and_reopen);
    RUN_TEST(test_ack_out_of_order);
    RUN_TEST(test_corrupt_crc_is_skipped);
    RUN_TEST(test_reboot_in_the_middle_of_a_write);
    RUN_TEST(test_corrupt_cursor_resends_everything);
    RUN_TEST(test_overflow_drops_oldest_segment);
    RUN_TEST(test_invalid_parameters);
    RUN_TEST(test_head_ack_with_full_out_of_order_list);
}
