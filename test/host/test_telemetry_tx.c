#include <stdio.h>
#include <string.h>

#include "telemetry_tx.h"
#include "test_support.h"

#define SEG_MAX   (4096U)
#define TOTAL_MAX (16384U)
#define BOOT_A    "aaaaaaaaaaaaaaaa"
#define LOG_MAX   (64U)

typedef struct {
    uint32_t sent_seq[LOG_MAX];
    uint32_t sent_count;
    telemetry_tx_event_t events[LOG_MAX];
    uint32_t event_count;
    bool refuse;
} capture_t;

static bool capture_publish(void *ctx, const char *payload, size_t len)
{
    capture_t *cap = (capture_t *)ctx;
    unsigned seq = 0;

    TEST_ASSERT_EQUAL_size_t(strlen(payload), len);
    if (cap->refuse) {
        return false;
    }
    TEST_ASSERT_EQUAL(1, sscanf(payload, "{\"seq\":%u", &seq));
    if (cap->sent_count < LOG_MAX) {
        cap->sent_seq[cap->sent_count] = seq;
    }
    cap->sent_count++;
    return true;
}

static void capture_event(void *ctx, telemetry_tx_event_t event, const journal_key_t *key)
{
    capture_t *cap = (capture_t *)ctx;
    (void)key;
    if (cap->event_count < LOG_MAX) {
        cap->events[cap->event_count] = event;
    }
    cap->event_count++;
}

typedef struct {
    char dir[64];
    journal_t journal;
    telemetry_tx_t tx;
    capture_t cap;
    char buf[JOURNAL_PAYLOAD_MAX + 1U];
} fixture_t;

static void fixture_open(fixture_t *f, uint32_t records)
{
    memset(f, 0, sizeof(*f));
    test_make_tmp_dir(f->dir, sizeof(f->dir));
    TEST_ASSERT_EQUAL(FW_OK, journal_open(&f->journal, f->dir, TOTAL_MAX, SEG_MAX));
    for (uint32_t i = 0; i < records; i++) {
        char payload[32];
        journal_key_t key;
        memset(&key, 0, sizeof(key));
        (void)snprintf(key.boot_id, sizeof(key.boot_id), "%s", BOOT_A);
        key.seq = i;
        const int32_t len = snprintf(payload, sizeof(payload), "{\"seq\":%u}", (unsigned)i);
        TEST_ASSERT_EQUAL(FW_OK, journal_append(&f->journal, &key, payload, (size_t)len, NULL));
    }
    telemetry_tx_init(&f->tx, &f->journal, capture_publish, capture_event, &f->cap);
}

static void poll(fixture_t *f, uint64_t now_ms, bool connected)
{
    telemetry_tx_poll(&f->tx, now_ms, connected, f->buf, sizeof(f->buf));
}

static void ack(fixture_t *f, uint32_t seq, contract_ack_status_t status,
                contract_ack_reason_t reason, uint64_t now_ms)
{
    contract_telemetry_ack_t a;
    memset(&a, 0, sizeof(a));
    (void)snprintf(a.boot_id, sizeof(a.boot_id), "%s", BOOT_A);
    a.seq = seq;
    a.status = status;
    a.reason = reason;
    telemetry_tx_on_ack(&f->tx, &a, now_ms);
}

static void test_window_and_no_send_while_offline(void)
{
    fixture_t f;
    fixture_open(&f, 10);

    poll(&f, 1000, false);
    TEST_ASSERT_EQUAL_UINT32(0, f.cap.sent_count);

    /* Só os 8 mais antigos, em ordem. */
    poll(&f, 2000, true);
    TEST_ASSERT_EQUAL_UINT32(8, f.cap.sent_count);
    for (uint32_t i = 0; i < 8U; i++) {
        TEST_ASSERT_EQUAL_UINT32(i, f.cap.sent_seq[i]);
    }

    /* Antes de 30 s sem ACK não reenvia nem avança. */
    poll(&f, 20000, true);
    TEST_ASSERT_EQUAL_UINT32(8, f.cap.sent_count);

    test_remove_dir(f.dir);
}

static void test_puback_alone_does_not_free_and_timeout_resends(void)
{
    fixture_t f;
    fixture_open(&f, 3);

    poll(&f, 1000, true);
    TEST_ASSERT_EQUAL_UINT32(3, f.cap.sent_count);
    /* Publicar (PUBACK do broker) não tira nada do journal. */
    TEST_ASSERT_EQUAL_UINT32(3, f.journal.pending);

    /* 30 s sem ACK de aplicação: o MESMO lote volta, com os mesmos seq. */
    poll(&f, 1000 + TELEMETRY_TX_ACK_TIMEOUT_MS, true);
    TEST_ASSERT_EQUAL_UINT32(6, f.cap.sent_count);
    TEST_ASSERT_EQUAL_UINT32(0, f.cap.sent_seq[3]);
    TEST_ASSERT_EQUAL_UINT32(1, f.cap.sent_seq[4]);
    TEST_ASSERT_EQUAL_UINT32(2, f.cap.sent_seq[5]);
    TEST_ASSERT_EQUAL_UINT32(3, f.journal.pending);

    test_remove_dir(f.dir);
}

static void test_stored_and_duplicate_free_the_window(void)
{
    fixture_t f;
    fixture_open(&f, 10);

    poll(&f, 1000, true);
    ack(&f, 1, CONTRACT_ACK_ST_STORED, CONTRACT_ACK_RSN_NONE, 1500); /* fora de ordem */
    ack(&f, 0, CONTRACT_ACK_ST_DUPLICATE, CONTRACT_ACK_RSN_NONE, 1600);
    TEST_ASSERT_EQUAL_UINT32(8, f.journal.pending);

    /* Dois lugares livres: entram o 8 e o 9, e ninguém é repetido. */
    poll(&f, 2000, true);
    TEST_ASSERT_EQUAL_UINT32(10, f.cap.sent_count);
    TEST_ASSERT_EQUAL_UINT32(8, f.cap.sent_seq[8]);
    TEST_ASSERT_EQUAL_UINT32(9, f.cap.sent_seq[9]);

    test_remove_dir(f.dir);
}

static void test_reconnect_resends_pending(void)
{
    fixture_t f;
    fixture_open(&f, 2);

    poll(&f, 1000, true);
    TEST_ASSERT_EQUAL_UINT32(2, f.cap.sent_count);

    telemetry_tx_on_connect(&f.tx);
    poll(&f, 2000, true);
    TEST_ASSERT_EQUAL_UINT32(4, f.cap.sent_count);
    TEST_ASSERT_EQUAL_UINT32(0, f.cap.sent_seq[2]);

    test_remove_dir(f.dir);
}

static void test_rejected_invalid_drops_batch(void)
{
    fixture_t f;
    fixture_open(&f, 2);

    poll(&f, 1000, true);
    ack(&f, 0, CONTRACT_ACK_ST_REJECTED, CONTRACT_ACK_RSN_INVALID, 1500);

    TEST_ASSERT_EQUAL_UINT32(1, f.journal.pending);
    TEST_ASSERT_EQUAL_UINT32(1, f.cap.event_count);
    TEST_ASSERT_EQUAL(TELEMETRY_TX_EV_REJECTED_INVALID, f.cap.events[0]);

    test_remove_dir(f.dir);
}

static void test_rejected_unclaimed_keeps_and_backs_off(void)
{
    fixture_t f;
    fixture_open(&f, 3);

    poll(&f, 1000, true);
    TEST_ASSERT_EQUAL_UINT32(3, f.cap.sent_count);

    /* A janela inteira é recusada de uma vez: conta como UMA espera. */
    for (uint32_t i = 0; i < 3U; i++) {
        ack(&f, i, CONTRACT_ACK_ST_REJECTED, CONTRACT_ACK_RSN_UNCLAIMED, 1500);
    }
    TEST_ASSERT_EQUAL_UINT32(3, f.journal.pending); /* nada é apagado */
    TEST_ASSERT_EQUAL(TELEMETRY_TX_EV_UNCLAIMED, f.cap.events[0]);

    poll(&f, 1500 + TELEMETRY_TX_BACKOFF_START_MS - 1U, true);
    TEST_ASSERT_EQUAL_UINT32(3, f.cap.sent_count);

    poll(&f, 1500 + TELEMETRY_TX_BACKOFF_START_MS, true);
    TEST_ASSERT_EQUAL_UINT32(6, f.cap.sent_count);

    /* Segunda recusa: espera dobrada. */
    const uint64_t t2 = 1500 + TELEMETRY_TX_BACKOFF_START_MS + 100U;
    ack(&f, 0, CONTRACT_ACK_ST_REJECTED, CONTRACT_ACK_RSN_UNCLAIMED, t2);
    poll(&f, t2 + TELEMETRY_TX_BACKOFF_START_MS, true);
    TEST_ASSERT_EQUAL_UINT32(6, f.cap.sent_count);
    poll(&f, t2 + (2U * TELEMETRY_TX_BACKOFF_START_MS), true);
    TEST_ASSERT_EQUAL_UINT32(9, f.cap.sent_count);

    /* A espera nunca passa de 5 min. */
    uint64_t now = t2 + (2U * TELEMETRY_TX_BACKOFF_START_MS);
    for (uint32_t i = 0; i < 10U; i++) {
        now += 1U;
        ack(&f, 0, CONTRACT_ACK_ST_REJECTED, CONTRACT_ACK_RSN_UNCLAIMED, now);
        TEST_ASSERT_TRUE((f.tx.hold_until_ms - now) <= TELEMETRY_TX_BACKOFF_MAX_MS);
        now = f.tx.hold_until_ms;
    }
    TEST_ASSERT_EQUAL_UINT32(TELEMETRY_TX_BACKOFF_MAX_MS, f.tx.backoff_ms);

    /* Depois do claim (stored) a espera volta ao início, e o aceite é avisado
     * uma vez só. */
    const uint32_t events_before = f.cap.event_count;
    ack(&f, 0, CONTRACT_ACK_ST_STORED, CONTRACT_ACK_RSN_NONE, now);
    TEST_ASSERT_EQUAL_UINT32(events_before + 1U, f.cap.event_count);
    TEST_ASSERT_EQUAL(TELEMETRY_TX_EV_ACCEPTED, f.cap.events[events_before]);
    ack(&f, 1, CONTRACT_ACK_ST_STORED, CONTRACT_ACK_RSN_NONE, now);
    TEST_ASSERT_EQUAL_UINT32(events_before + 1U, f.cap.event_count);
    TEST_ASSERT_EQUAL_UINT32(TELEMETRY_TX_BACKOFF_START_MS, f.tx.backoff_ms);
    TEST_ASSERT_EQUAL_UINT32(1, f.journal.pending);

    test_remove_dir(f.dir);
}

static void test_rejected_clock_skew_keeps_and_asks_resync(void)
{
    fixture_t f;
    fixture_open(&f, 1);

    poll(&f, 1000, true);
    ack(&f, 0, CONTRACT_ACK_ST_REJECTED, CONTRACT_ACK_RSN_CLOCK_SKEW, 1500);
    TEST_ASSERT_EQUAL_UINT32(1, f.journal.pending);
    TEST_ASSERT_EQUAL(TELEMETRY_TX_EV_CLOCK_SKEW, f.cap.events[0]);

    poll(&f, 2000, true);
    TEST_ASSERT_EQUAL_UINT32(1, f.cap.sent_count);
    poll(&f, 1500 + TELEMETRY_TX_SKEW_HOLD_MS, true);
    TEST_ASSERT_EQUAL_UINT32(2, f.cap.sent_count);

    test_remove_dir(f.dir);
}

static void test_rejected_revoked_stops_until_reboot(void)
{
    fixture_t f;
    fixture_open(&f, 2);

    poll(&f, 1000, true);
    ack(&f, 0, CONTRACT_ACK_ST_REJECTED, CONTRACT_ACK_RSN_REVOKED, 1500);
    TEST_ASSERT_EQUAL(TELEMETRY_TX_EV_REVOKED, f.cap.events[0]);

    telemetry_tx_on_connect(&f.tx);
    poll(&f, 10000000, true);
    TEST_ASSERT_EQUAL_UINT32(2, f.cap.sent_count);
    TEST_ASSERT_EQUAL_UINT32(2, f.journal.pending);

    /* "Reboot": um transmissor novo volta a tentar. */
    telemetry_tx_init(&f.tx, &f.journal, capture_publish, capture_event, &f.cap);
    poll(&f, 1000, true);
    TEST_ASSERT_EQUAL_UINT32(4, f.cap.sent_count);

    test_remove_dir(f.dir);
}

static void test_full_publish_queue_retries_next_cycle(void)
{
    fixture_t f;
    fixture_open(&f, 2);

    f.cap.refuse = true;
    poll(&f, 1000, true);
    TEST_ASSERT_EQUAL_UINT32(0, f.cap.sent_count);

    f.cap.refuse = false;
    poll(&f, 1100, true);
    TEST_ASSERT_EQUAL_UINT32(2, f.cap.sent_count);

    test_remove_dir(f.dir);
}

void run_telemetry_tx_tests(void)
{
    RUN_TEST(test_window_and_no_send_while_offline);
    RUN_TEST(test_puback_alone_does_not_free_and_timeout_resends);
    RUN_TEST(test_stored_and_duplicate_free_the_window);
    RUN_TEST(test_reconnect_resends_pending);
    RUN_TEST(test_rejected_invalid_drops_batch);
    RUN_TEST(test_rejected_unclaimed_keeps_and_backs_off);
    RUN_TEST(test_rejected_clock_skew_keeps_and_asks_resync);
    RUN_TEST(test_rejected_revoked_stops_until_reboot);
    RUN_TEST(test_full_publish_queue_retries_next_cycle);
}
