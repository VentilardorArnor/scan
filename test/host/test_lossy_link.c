#include <stdio.h>
#include <string.h>

#include "telemetry_tx.h"
#include "test_support.h"

/*
 * Simulação de um enlace ruim entre o journal da placa e o servidor, para
 * provar que o envio nunca trava de vez:
 *
 * - publicações perdidas (o servidor nem vê o lote);
 * - ACKs perdidos, inclusive o do lote mais antigo por 10 min seguidos
 *   (o cenário que travou a placa em 23/10);
 * - ACKs duplicados (QoS 1 entrega "pelo menos uma vez");
 * - ACKs fora de ordem e atrasados de um ciclo para outro;
 * - rajadas maiores que a lista de ACKs fora de ordem (32).
 *
 * O servidor simulado grava cada (boot_id, seq) uma vez só e responde
 * `duplicate` para o que já tem, como o pivo-saas.
 */

#define SIM_RECORDS        (150U)
#define SIM_TICK_MS        (1000U)
#define SIM_MAX_MS         (3ULL * 3600ULL * 1000ULL) /* 3 h simuladas */
#define HEAD_BLACKOUT_MS   (600000ULL)                /* 10 min sem ACK do lote 0 */
#define TRANSIT_MAX        (4096U)
#define SEG_MAX            (4096U)
#define TOTAL_MAX          (65536U)
#define BOOT_A             "aaaaaaaaaaaaaaaa"

typedef struct {
    uint32_t seq;
    bool duplicate; /* resposta do servidor para este ACK */
} sim_ack_t;

typedef struct {
    uint32_t published[TRANSIT_MAX]; /* publicações ainda não processadas pelo servidor */
    uint32_t published_count;
    bool stored[SIM_RECORDS];
    uint32_t stored_count;
    uint32_t store_events; /* quantas vezes o servidor gravou (deve ser 1 por lote) */
    sim_ack_t held[TRANSIT_MAX];     /* ACKs atrasados para o próximo ciclo */
    uint32_t held_count;
    uint32_t rng;
} sim_t;

static uint32_t rnd(sim_t *s, uint32_t mod)
{
    s->rng = (s->rng * 1103515245U) + 12345U; /* LCG determinístico */
    return (s->rng >> 16) % mod;
}

static bool sim_publish(void *ctx, const char *payload, size_t len)
{
    sim_t *s = (sim_t *)ctx;
    unsigned seq = 0;
    (void)len;
    TEST_ASSERT_EQUAL(1, sscanf(payload, "{\"seq\":%u", &seq));
    TEST_ASSERT_TRUE(s->published_count < TRANSIT_MAX);
    s->published[s->published_count++] = seq;
    return true;
}

static void sim_event(void *ctx, telemetry_tx_event_t event, const journal_key_t *key)
{
    (void)ctx;
    (void)event;
    (void)key;
}

static void deliver(telemetry_tx_t *tx, uint32_t seq, bool duplicate, uint64_t now_ms)
{
    contract_telemetry_ack_t a;
    memset(&a, 0, sizeof(a));
    (void)snprintf(a.boot_id, sizeof(a.boot_id), "%s", BOOT_A);
    a.seq = seq;
    a.status = duplicate ? CONTRACT_ACK_ST_DUPLICATE : CONTRACT_ACK_ST_STORED;
    telemetry_tx_on_ack(tx, &a, now_ms);
}

static void test_lossy_link_never_deadlocks(void)
{
    char dir[64];
    static journal_t journal;
    static telemetry_tx_t tx;
    static sim_t sim;
    static sim_ack_t batch[TRANSIT_MAX];
    char buf[JOURNAL_PAYLOAD_MAX + 1U];
    uint8_t max_acked = 0;
    uint64_t now = 0;

    memset(&sim, 0, sizeof(sim));
    sim.rng = 20261023U;

    test_make_tmp_dir(dir, sizeof(dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&journal, dir, TOTAL_MAX, SEG_MAX));
    for (uint32_t i = 0; i < SIM_RECORDS; i++) {
        char payload[32];
        journal_key_t key;
        memset(&key, 0, sizeof(key));
        (void)snprintf(key.boot_id, sizeof(key.boot_id), "%s", BOOT_A);
        key.seq = i;
        const int32_t len = snprintf(payload, sizeof(payload), "{\"seq\":%u}", (unsigned)i);
        TEST_ASSERT_EQUAL(FW_OK, journal_append(&journal, &key, payload, (size_t)len, NULL));
    }
    telemetry_tx_init(&tx, &journal, sim_publish, sim_event, &sim);

    for (now = 0; (now < SIM_MAX_MS) && (journal.pending > 0U); now += SIM_TICK_MS) {
        telemetry_tx_poll(&tx, now, true, buf, sizeof(buf));

        /* Servidor: processa as publicações que chegaram (10% se perdem). */
        uint32_t n = 0;
        for (uint32_t i = 0; i < sim.published_count; i++) {
            const uint32_t seq = sim.published[i];
            if (rnd(&sim, 100) < 10U) {
                continue; /* publicação perdida */
            }
            const bool dup = sim.stored[seq];
            if (!dup) {
                sim.stored[seq] = true;
                sim.stored_count++;
                sim.store_events++;
            }
            const bool head_blackout = (seq == 0U) && (now < HEAD_BLACKOUT_MS);
            if (head_blackout || (rnd(&sim, 100) < 25U)) {
                continue; /* ACK perdido */
            }
            batch[n++] = (sim_ack_t){seq, dup};
            if (rnd(&sim, 100) < 15U) {
                batch[n++] = (sim_ack_t){seq, true}; /* ACK repetido */
            }
        }
        sim.published_count = 0;

        /* ACKs atrasados do ciclo anterior entram junto, e tudo embaralhado. */
        for (uint32_t i = 0; i < sim.held_count; i++) {
            batch[n++] = sim.held[i];
        }
        sim.held_count = 0;
        for (uint32_t i = n; i > 1U; i--) {
            const uint32_t k = rnd(&sim, i);
            const sim_ack_t t = batch[i - 1U];
            batch[i - 1U] = batch[k];
            batch[k] = t;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (rnd(&sim, 100) < 20U) {
                sim.held[sim.held_count++] = batch[i]; /* chega no próximo ciclo */
            } else {
                deliver(&tx, batch[i].seq, batch[i].duplicate, now);
            }
            if (journal.acked_count > max_acked) {
                max_acked = journal.acked_count;
            }
        }
    }

    /* Nada travou: tudo saiu do journal e o servidor gravou cada lote 1 vez. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, journal.pending, "journal travou");
    TEST_ASSERT_EQUAL_UINT32(SIM_RECORDS, sim.stored_count);
    TEST_ASSERT_EQUAL_UINT32(SIM_RECORDS, sim.store_events);
    TEST_ASSERT_EQUAL_UINT8(0, journal.acked_count);
    /* O cenário que travava a placa foi de fato exercitado: lista cheia. */
    TEST_ASSERT_EQUAL_UINT8(JOURNAL_ACKED_MAX, max_acked);
    TEST_ASSERT_TRUE(tx.stats.ack_other_err > 0U);    /* ACKs recusados com a lista cheia */
    TEST_ASSERT_TRUE(tx.stats.ack_not_found > 0U);    /* ACKs repetidos de lotes já liberados */
    TEST_ASSERT_TRUE(tx.stats.resent > 0U);
    /* Terminou bem depois do bloqueio do lote 0, mas dentro do limite. */
    TEST_ASSERT_TRUE(now > HEAD_BLACKOUT_MS);

    /* Estado persistido coerente após "reboot". */
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&journal, dir, TOTAL_MAX, SEG_MAX));
    TEST_ASSERT_EQUAL_UINT32(0, journal.pending);
    test_remove_dir(dir);
}

void run_lossy_link_tests(void)
{
    RUN_TEST(test_lossy_link_never_deadlocks);
}
