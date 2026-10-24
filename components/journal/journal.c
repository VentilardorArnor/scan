#include "journal.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_crc32.h"

#define REC_MAGIC          (0x314E524AU) /* "JRN1" em little-endian */
#define REC_VERSION        (1U)
#define REC_HEADER_LEN     (27U) /* magic 4 + versão 1 + tamanho 2 + boot_id 16 + seq 4 */
#define REC_CRC_LEN        (4U)
#define REC_CRC_START      (4U)  /* o CRC começa depois do magic */
#define REC_OFF_VERSION    (4U)
#define REC_OFF_LEN        (5U)
#define REC_OFF_BOOT_ID    (7U)
#define REC_OFF_SEQ        (23U)

#define CURSOR_MAGIC       (0x3152434AU) /* "JCR1" */
#define CURSOR_FIXED_LEN   (17U) /* magic 4 + seg 4 + off 4 + dropped 4 + count 1 */
#define CURSOR_ENTRY_LEN   (JOURNAL_BOOT_ID_LEN + 4U)
#define CURSOR_MAX_LEN     (CURSOR_FIXED_LEN + (JOURNAL_ACKED_MAX * CURSOR_ENTRY_LEN) + REC_CRC_LEN)

#define PATH_MAX_LEN       (JOURNAL_DIR_MAX + 24U)
#define SEG_NAME_FMT       "seg-%08lu.jnl"
#define SEG_NAME_PREFIX    "seg-"
#define SEG_NAME_LEN       (16U)
#define SEG_FIRST          (1U)
#define CURSOR_NAME        "cursor.bin"
#define CURSOR_TMP_NAME    "cursor.tmp"
#define DIR_MODE           (0775)

/* Área de trabalho para ler payloads. O journal tem uma única tarefa dona
 * (journal_task), então um buffer estático é seguro e evita 2 KB de pilha. */
static uint8_t s_scratch[JOURNAL_PAYLOAD_MAX];

/* ------------------------------------------------------------------ */
/* Auxiliares                                                          */
/* ------------------------------------------------------------------ */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void seg_path(const journal_t *j, uint32_t seg, char out[PATH_MAX_LEN])
{
    (void)snprintf(out, PATH_MAX_LEN, "%s/" SEG_NAME_FMT, j->dir, (unsigned long)seg);
}

static void named_path(const journal_t *j, const char *name, char out[PATH_MAX_LEN])
{
    (void)snprintf(out, PATH_MAX_LEN, "%s/%s", j->dir, name);
}

static uint32_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (uint32_t)st.st_size;
}

static bool key_equal(const journal_key_t *a, const journal_key_t *b)
{
    return (a->seq == b->seq) && (strncmp(a->boot_id, b->boot_id, JOURNAL_BOOT_ID_LEN) == 0);
}

static int32_t acked_find(const journal_t *j, const journal_key_t *key)
{
    for (uint8_t i = 0; i < j->acked_count; i++) {
        if (key_equal(&j->acked[i], key)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void acked_remove(journal_t *j, int32_t index)
{
    j->acked[index] = j->acked[j->acked_count - 1U];
    j->acked_count--;
}

/* ------------------------------------------------------------------ */
/* Varredura crua (não pula registros com ACK)                         */
/* ------------------------------------------------------------------ */

static void iter_close(journal_iter_t *it)
{
    if (it->file != NULL) {
        (void)fclose(it->file);
        it->file = NULL;
    }
}

static void iter_next_segment(journal_iter_t *it)
{
    iter_close(it);
    it->seg++;
    it->off = 0;
    it->in_garbage = false;
}

static void iter_mark_garbage(journal_iter_t *it)
{
    if (!it->in_garbage) {
        it->in_garbage = true;
        it->garbage_runs++;
    }
}

/* Lê o próximo registro íntegro a partir de (seg, off). Trecho inválido é
 * pulado byte a byte até achar um registro com magic e CRC corretos: é o que
 * permite sobreviver a um registro cortado por reset no meio da escrita. */
static fw_err_t raw_next(const journal_t *j, journal_iter_t *it, journal_record_t *rec,
                         char *payload, size_t cap)
{
    uint8_t header[REC_HEADER_LEN];
    uint8_t crc_bytes[REC_CRC_LEN];
    char path[PATH_MAX_LEN];

    for (;;) {
        if (it->file == NULL) {
            if (it->seg > j->write_seg) {
                return FW_ERR_NOT_FOUND;
            }
            seg_path(j, it->seg, path);
            it->file = fopen(path, "rb");
            if ((it->file == NULL) || (fseek(it->file, (long)it->off, SEEK_SET) != 0)) {
                iter_next_segment(it);
                continue;
            }
        }

        const size_t got = fread(header, 1, REC_HEADER_LEN, it->file);
        if (got < REC_HEADER_LEN) {
            if (got > 0U) {
                iter_mark_garbage(it); /* cabeçalho cortado no fim do segmento */
            }
            iter_next_segment(it);
            continue;
        }

        const uint16_t len = get_u16(&header[REC_OFF_LEN]);
        bool ok = (get_u32(header) == REC_MAGIC) && (header[REC_OFF_VERSION] == REC_VERSION) &&
                  (len > 0U) && (len <= JOURNAL_PAYLOAD_MAX);

        if (ok) {
            ok = (fread(s_scratch, 1, len, it->file) == len) &&
                 (fread(crc_bytes, 1, REC_CRC_LEN, it->file) == REC_CRC_LEN);
        }
        if (ok) {
            uint32_t crc = fw_crc32(FW_CRC32_INIT, &header[REC_CRC_START],
                                    REC_HEADER_LEN - REC_CRC_START);
            crc = fw_crc32(crc, s_scratch, len);
            ok = (crc == get_u32(crc_bytes));
        }

        if (!ok) {
            iter_mark_garbage(it);
            it->off++;
            if (fseek(it->file, (long)it->off, SEEK_SET) != 0) {
                iter_next_segment(it);
            }
            continue;
        }

        it->in_garbage = false;
        it->off += REC_HEADER_LEN + (uint32_t)len + REC_CRC_LEN;

        memcpy(rec->key.boot_id, &header[REC_OFF_BOOT_ID], JOURNAL_BOOT_ID_LEN);
        rec->key.boot_id[JOURNAL_BOOT_ID_LEN] = '\0';
        rec->key.seq = get_u32(&header[REC_OFF_SEQ]);
        rec->len = len;

        if (payload != NULL) {
            if (cap <= (size_t)len) {
                return FW_ERR_NO_MEM; /* já avançou: o chamador não fica preso */
            }
            memcpy(payload, s_scratch, len);
            payload[len] = '\0';
        }
        return FW_OK;
    }
}

/* ------------------------------------------------------------------ */
/* Cursor                                                              */
/* ------------------------------------------------------------------ */

static fw_err_t cursor_save(const journal_t *j)
{
    uint8_t buf[CURSOR_MAX_LEN];
    char tmp_path[PATH_MAX_LEN];
    char final_path[PATH_MAX_LEN];
    size_t len = 0;

    put_u32(&buf[0], CURSOR_MAGIC);
    put_u32(&buf[4], j->cur_seg);
    put_u32(&buf[8], j->cur_off);
    put_u32(&buf[12], j->dropped);
    buf[16] = j->acked_count;
    len = CURSOR_FIXED_LEN;
    for (uint8_t i = 0; i < j->acked_count; i++) {
        memcpy(&buf[len], j->acked[i].boot_id, JOURNAL_BOOT_ID_LEN);
        put_u32(&buf[len + JOURNAL_BOOT_ID_LEN], j->acked[i].seq);
        len += CURSOR_ENTRY_LEN;
    }
    put_u32(&buf[len], fw_crc32(FW_CRC32_INIT, buf, len));
    len += REC_CRC_LEN;

    named_path(j, CURSOR_TMP_NAME, tmp_path);
    named_path(j, CURSOR_NAME, final_path);

    /* Grava em arquivo temporário e troca com rename: uma queda de energia
     * deixa o cursor antigo ou o novo, nunca um pela metade. */
    FILE *file = fopen(tmp_path, "wb");
    if (file == NULL) {
        return FW_ERR_IO;
    }
    bool ok = (fwrite(buf, 1, len, file) == len) && (fflush(file) == 0) &&
              (fsync(fileno(file)) == 0);
    ok = (fclose(file) == 0) && ok;
    if (!ok || (rename(tmp_path, final_path) != 0)) {
        (void)remove(tmp_path);
        return FW_ERR_IO;
    }
    return FW_OK;
}

static bool cursor_load(journal_t *j)
{
    uint8_t buf[CURSOR_MAX_LEN];
    char path[PATH_MAX_LEN];

    named_path(j, CURSOR_NAME, path);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    const size_t len = fread(buf, 1, sizeof(buf), file);
    (void)fclose(file);

    if ((len < (CURSOR_FIXED_LEN + REC_CRC_LEN)) || (get_u32(buf) != CURSOR_MAGIC)) {
        return false;
    }
    const uint8_t count = buf[16];
    const size_t body_len = CURSOR_FIXED_LEN + ((size_t)count * CURSOR_ENTRY_LEN);
    if ((count > JOURNAL_ACKED_MAX) || (len != (body_len + REC_CRC_LEN)) ||
        (fw_crc32(FW_CRC32_INIT, buf, body_len) != get_u32(&buf[body_len]))) {
        return false;
    }

    j->cur_seg = get_u32(&buf[4]);
    j->cur_off = get_u32(&buf[8]);
    j->dropped = get_u32(&buf[12]);
    j->acked_count = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *entry = &buf[CURSOR_FIXED_LEN + ((size_t)i * CURSOR_ENTRY_LEN)];
        memcpy(j->acked[i].boot_id, entry, JOURNAL_BOOT_ID_LEN);
        j->acked[i].boot_id[JOURNAL_BOOT_ID_LEN] = '\0';
        j->acked[i].seq = get_u32(&entry[JOURNAL_BOOT_ID_LEN]);
    }
    return true;
}

/* Apaga os segmentos que ficaram inteiros antes do cursor. */
static void delete_consumed_segments(journal_t *j)
{
    char path[PATH_MAX_LEN];

    while (j->first_seg < j->cur_seg) {
        seg_path(j, j->first_seg, path);
        const uint32_t size = file_size(path);
        if (remove(path) == 0) {
            j->total_bytes -= (size <= j->total_bytes) ? size : j->total_bytes;
        }
        j->first_seg++;
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

static bool parse_seg_name(const char *name, uint32_t *seg)
{
    if ((strlen(name) != SEG_NAME_LEN) || (strncmp(name, SEG_NAME_PREFIX, 4) != 0) ||
        (strcmp(&name[12], ".jnl") != 0)) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 4; i < 12U; i++) {
        if ((name[i] < '0') || (name[i] > '9')) {
            return false;
        }
        value = (value * 10U) + (uint32_t)(name[i] - '0');
    }
    *seg = value;
    return true;
}

fw_err_t journal_open(journal_t *j, const char *dir, uint32_t max_bytes, uint32_t seg_max_bytes)
{
    char path[PATH_MAX_LEN];

    if ((j == NULL) || (dir == NULL) || (strlen(dir) >= JOURNAL_DIR_MAX) ||
        (seg_max_bytes < (REC_HEADER_LEN + JOURNAL_PAYLOAD_MAX + REC_CRC_LEN)) ||
        (max_bytes < (2U * seg_max_bytes))) {
        return FW_ERR_INVALID_PARAM;
    }

    memset(j, 0, sizeof(*j));
    (void)snprintf(j->dir, sizeof(j->dir), "%s", dir);
    j->max_bytes = max_bytes;
    j->seg_max_bytes = seg_max_bytes;

    (void)mkdir(dir, DIR_MODE); /* já existe na placa (raiz do LittleFS) */

    DIR *handle = opendir(dir);
    if (handle == NULL) {
        return FW_ERR_IO;
    }

    bool any = false;
    uint32_t min_seg = 0;
    uint32_t max_seg = 0;
    const struct dirent *entry = readdir(handle);
    while (entry != NULL) {
        uint32_t seg = 0;
        if (parse_seg_name(entry->d_name, &seg)) {
            named_path(j, entry->d_name, path);
            j->total_bytes += file_size(path);
            if (!any || (seg < min_seg)) {
                min_seg = seg;
            }
            if (!any || (seg > max_seg)) {
                max_seg = seg;
            }
            any = true;
        }
        entry = readdir(handle);
    }
    (void)closedir(handle);

    named_path(j, CURSOR_TMP_NAME, path);
    (void)remove(path); /* sobra de uma troca de cursor interrompida */

    j->first_seg = any ? min_seg : SEG_FIRST;
    j->write_seg = any ? (max_seg + 1U) : SEG_FIRST;
    j->write_off = 0;

    /* Cursor ausente ou corrompido: recomeça do registro mais antigo. Reenviar
     * é seguro, o servidor responde `duplicate` para (boot_id, seq) repetido. */
    const bool cursor_ok = cursor_load(j);
    if (!cursor_ok || (j->cur_seg < j->first_seg) || (j->cur_seg > j->write_seg)) {
        j->cur_seg = j->first_seg;
        j->cur_off = 0;
        j->acked_count = 0;
    }
    delete_consumed_segments(j);

    /* Varredura: conta pendentes, detecta corrupção e descarta ACKs órfãos. */
    journal_iter_t it;
    journal_record_t rec;
    uint32_t seen_mask = 0;
    journal_iter_begin(j, &it);
    while (raw_next(j, &it, &rec, NULL, 0) == FW_OK) {
        const int32_t index = acked_find(j, &rec.key);
        if (index >= 0) {
            seen_mask |= (1UL << (uint32_t)index);
        } else {
            j->pending++;
        }
    }
    j->corrupt_found = it.garbage_runs;
    journal_iter_end(&it);

    for (int32_t i = (int32_t)j->acked_count - 1; i >= 0; i--) {
        if ((seen_mask & (1UL << (uint32_t)i)) == 0U) {
            acked_remove(j, i);
        }
    }

    return FW_OK;
}

/* Apaga o segmento mais antigo para abrir espaço; conta o que se perdeu. */
static uint32_t drop_oldest_segment(journal_t *j)
{
    char path[PATH_MAX_LEN];
    const uint32_t seg = j->first_seg;
    uint32_t lost = 0;

    journal_iter_t it = {
        .seg = seg,
        .off = (j->cur_seg == seg) ? j->cur_off : 0U,
    };
    journal_record_t rec;
    while ((it.seg == seg) && (raw_next(j, &it, &rec, NULL, 0) == FW_OK) && (it.seg == seg)) {
        const int32_t index = acked_find(j, &rec.key);
        if (index >= 0) {
            acked_remove(j, index); /* já tinha saído de `pending` */
        } else {
            lost++;
        }
    }
    journal_iter_end(&it);

    seg_path(j, seg, path);
    const uint32_t size = file_size(path);
    (void)remove(path);
    j->total_bytes -= (size <= j->total_bytes) ? size : j->total_bytes;
    j->first_seg = seg + 1U;
    if (j->cur_seg <= seg) {
        j->cur_seg = seg + 1U;
        j->cur_off = 0;
    }
    j->pending -= (lost <= j->pending) ? lost : j->pending;
    j->dropped += lost;
    return lost;
}

fw_err_t journal_append(journal_t *j, const journal_key_t *key, const char *payload, size_t len,
                        uint32_t *dropped_now)
{
    uint8_t header[REC_HEADER_LEN];
    uint8_t crc_bytes[REC_CRC_LEN];
    char path[PATH_MAX_LEN];

    if (dropped_now != NULL) {
        *dropped_now = 0;
    }
    if ((j == NULL) || (key == NULL) || (payload == NULL) || (len == 0U) ||
        (len > JOURNAL_PAYLOAD_MAX)) {
        return FW_ERR_INVALID_PARAM;
    }

    const uint32_t rec_size = REC_HEADER_LEN + (uint32_t)len + REC_CRC_LEN;

    if ((j->write_off > 0U) && ((j->write_off + rec_size) > j->seg_max_bytes)) {
        j->write_seg++;
        j->write_off = 0;
    }

    /* Overflow: sai o segmento mais antigo. Nunca apaga o que está em escrita. */
    uint32_t lost = 0;
    while (((j->total_bytes + rec_size) > j->max_bytes) && (j->first_seg < j->write_seg)) {
        lost += drop_oldest_segment(j);
    }
    if (lost > 0U) {
        (void)cursor_save(j); /* `dropped` e o cursor novo; falha aqui só causa reenvio */
        if (dropped_now != NULL) {
            *dropped_now = lost;
        }
    }

    memset(header, 0, sizeof(header));
    put_u32(&header[0], REC_MAGIC);
    header[REC_OFF_VERSION] = REC_VERSION;
    put_u16(&header[REC_OFF_LEN], (uint16_t)len);
    (void)strncpy((char *)&header[REC_OFF_BOOT_ID], key->boot_id, JOURNAL_BOOT_ID_LEN);
    put_u32(&header[REC_OFF_SEQ], key->seq);

    uint32_t crc = fw_crc32(FW_CRC32_INIT, &header[REC_CRC_START], REC_HEADER_LEN - REC_CRC_START);
    crc = fw_crc32(crc, (const uint8_t *)payload, len);
    put_u32(crc_bytes, crc);

    seg_path(j, j->write_seg, path);
    FILE *file = fopen(path, "ab");
    if (file == NULL) {
        return FW_ERR_IO;
    }

    /* flush + fsync antes de considerar gravado: o envelope precisa
     * sobreviver a uma queda de energia logo depois. */
    bool ok = (fwrite(header, 1, REC_HEADER_LEN, file) == REC_HEADER_LEN) &&
              (fwrite(payload, 1, len, file) == len) &&
              (fwrite(crc_bytes, 1, REC_CRC_LEN, file) == REC_CRC_LEN) && (fflush(file) == 0) &&
              (fsync(fileno(file)) == 0);
    ok = (fclose(file) == 0) && ok;

    /* O tamanho real do arquivo manda: se a escrita falhou no meio, o resto
     * vira lixo que a varredura pula. */
    const uint32_t new_size = file_size(path);
    if (new_size >= j->write_off) {
        j->total_bytes += new_size - j->write_off;
    }
    j->write_off = new_size;

    if (!ok) {
        return FW_ERR_IO;
    }
    j->pending++;
    return FW_OK;
}

fw_err_t journal_ack(journal_t *j, const journal_key_t *key)
{
    if ((j == NULL) || (key == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }
    if (acked_find(j, key) >= 0) {
        return FW_OK; /* ACK repetido */
    }

    /* Confirma que a chave existe entre os pendentes mais antigos. Um ACK de
     * outro boot ou já consolidado não pode ocupar a lista. */
    journal_iter_t it;
    journal_record_t rec;
    bool found = false;
    bool is_head = false;
    journal_iter_begin(j, &it);
    for (uint32_t i = 0; (i < JOURNAL_ACK_SEARCH_MAX) && !found; i++) {
        if (raw_next(j, &it, &rec, NULL, 0) != FW_OK) {
            break;
        }
        found = key_equal(&rec.key, key);
        is_head = found && (i == 0U);
    }
    journal_iter_end(&it);
    if (!found) {
        return FW_ERR_NOT_FOUND;
    }

    /* O registro da frente do cursor é consumido direto e não ocupa a lista.
     * Sem isso, com a lista cheia de ACKs fora de ordem, justamente o ACK
     * que destravaria o cursor seria recusado para sempre. */
    if (!is_head) {
        if (j->acked_count >= JOURNAL_ACKED_MAX) {
            return FW_ERR_FULL; /* sem perda: o lote será reenviado e virá `duplicate` */
        }
        j->acked[j->acked_count] = *key;
        j->acked_count++;
    }
    if (j->pending > 0U) {
        j->pending--;
    }

    /* Avança o cursor enquanto o registro da frente já tiver ACK. */
    bool head_pending = is_head;
    journal_iter_begin(j, &it);
    for (;;) {
        const fw_err_t err = raw_next(j, &it, &rec, NULL, 0);
        if (err == FW_ERR_NOT_FOUND) {
            j->cur_seg = j->write_seg; /* tudo confirmado */
            j->cur_off = j->write_off;
            break;
        }
        if (head_pending && key_equal(&rec.key, key)) {
            head_pending = false; /* o próprio registro deste ACK */
        } else {
            const int32_t index = acked_find(j, &rec.key);
            if (index < 0) {
                break;
            }
            acked_remove(j, index);
        }
        j->cur_seg = it.seg;
        j->cur_off = it.off;
    }
    journal_iter_end(&it);

    /* Primeiro o cursor, depois os arquivos: se cair entre os dois, a próxima
     * abertura apaga os segmentos já consumidos. */
    const fw_err_t err = cursor_save(j);
    if (err == FW_OK) {
        delete_consumed_segments(j);
    }
    return err;
}

void journal_iter_begin(const journal_t *j, journal_iter_t *it)
{
    if ((j == NULL) || (it == NULL)) {
        return;
    }
    memset(it, 0, sizeof(*it));
    it->seg = j->cur_seg;
    it->off = j->cur_off;
}

fw_err_t journal_iter_next(const journal_t *j, journal_iter_t *it, journal_record_t *rec,
                           char *payload, size_t cap)
{
    if ((j == NULL) || (it == NULL) || (rec == NULL)) {
        return FW_ERR_INVALID_PARAM;
    }
    for (;;) {
        const fw_err_t err = raw_next(j, it, rec, payload, cap);
        if (err != FW_OK) {
            return err;
        }
        if (acked_find(j, &rec->key) < 0) {
            return FW_OK;
        }
    }
}

void journal_iter_end(journal_iter_t *it)
{
    if (it != NULL) {
        iter_close(it);
    }
}
