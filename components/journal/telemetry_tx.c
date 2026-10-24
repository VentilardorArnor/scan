#include "telemetry_tx.h"

#include <string.h>

static bool key_equal(const journal_key_t *a, const journal_key_t *b)
{
    return (a->seq == b->seq) && (strcmp(a->boot_id, b->boot_id) == 0);
}

static void notify(telemetry_tx_t *tx, telemetry_tx_event_t event, const journal_key_t *key)
{
    if (tx->on_event != NULL) {
        tx->on_event(tx->ctx, event, key);
    }
}

void telemetry_tx_init(telemetry_tx_t *tx, journal_t *journal, telemetry_tx_publish_fn publish,
                       telemetry_tx_event_fn on_event, void *ctx)
{
    if (tx == NULL) {
        return;
    }
    memset(tx, 0, sizeof(*tx));
    tx->journal = journal;
    tx->publish = publish;
    tx->on_event = on_event;
    tx->ctx = ctx;
    tx->backoff_ms = TELEMETRY_TX_BACKOFF_START_MS;
}

void telemetry_tx_poll(telemetry_tx_t *tx, uint64_t now_ms, bool connected, char *payload_buf,
                       size_t payload_cap)
{
    if ((tx == NULL) || (tx->journal == NULL) || (tx->publish == NULL) || (payload_buf == NULL)) {
        return;
    }
    if (!connected || tx->revoked || (now_ms < tx->hold_until_ms)) {
        return;
    }

    /* A janela é sempre os N pendentes mais antigos, em ordem de gravação
     * (que é a ordem crescente de boot_id/seq). */
    telemetry_tx_slot_t next[TELEMETRY_TX_WINDOW];
    memset(next, 0, sizeof(next));

    journal_iter_t it;
    journal_record_t rec;
    journal_iter_begin(tx->journal, &it);

    for (uint32_t n = 0; n < TELEMETRY_TX_WINDOW; n++) {
        const fw_err_t err = journal_iter_next(tx->journal, &it, &rec, payload_buf, payload_cap);
        if (err == FW_ERR_NOT_FOUND) {
            break;
        }
        if (err != FW_OK) {
            continue; /* não coube na área de trabalho: fica para trás */
        }

        next[n].key = rec.key;
        next[n].used = true;

        bool in_flight = false;
        bool was_sent = false;
        for (uint32_t i = 0; i < TELEMETRY_TX_WINDOW; i++) {
            if (tx->slots[i].used && key_equal(&tx->slots[i].key, &rec.key)) {
                next[n].sent_ms = tx->slots[i].sent_ms;
                in_flight = (now_ms - tx->slots[i].sent_ms) < TELEMETRY_TX_ACK_TIMEOUT_MS;
                was_sent = true;
                break;
            }
        }

        if (!in_flight) {
            if (tx->publish(tx->ctx, payload_buf, rec.len)) {
                next[n].sent_ms = now_ms;
                if (was_sent) {
                    tx->stats.resent++;
                } else {
                    tx->stats.sent++;
                }
            } else {
                tx->stats.publish_refused++;
                next[n].used = false; /* fila cheia: tenta de novo no próximo ciclo */
            }
        }
    }

    journal_iter_end(&it);
    memcpy(tx->slots, next, sizeof(tx->slots));
}

void telemetry_tx_on_ack(telemetry_tx_t *tx, const contract_telemetry_ack_t *ack, uint64_t now_ms)
{
    if ((tx == NULL) || (tx->journal == NULL) || (ack == NULL)) {
        return;
    }

    journal_key_t key;
    memset(&key, 0, sizeof(key));
    if (strlen(ack->boot_id) > JOURNAL_BOOT_ID_LEN) {
        return; /* não é um boot_id gerado por este firmware */
    }
    (void)strncpy(key.boot_id, ack->boot_id, JOURNAL_BOOT_ID_LEN);
    key.seq = ack->seq;

    switch (ack->status) {
    case CONTRACT_ACK_ST_STORED:
    case CONTRACT_ACK_ST_DUPLICATE:
        if (ack->status == CONTRACT_ACK_ST_STORED) {
            tx->stats.ack_stored++;
        } else {
            tx->stats.ack_duplicate++;
        }
        {
            const fw_err_t err = journal_ack(tx->journal, &key);
            if (err == FW_ERR_NOT_FOUND) {
                tx->stats.ack_not_found++;
            } else if (err != FW_OK) {
                tx->stats.ack_other_err++;
                tx->stats.last_ack_err = (int32_t)err;
            } else {
                /* liberado */
            }
        }
        tx->backoff_ms = TELEMETRY_TX_BACKOFF_START_MS;
        if (!tx->accepting) {
            tx->accepting = true;
            notify(tx, TELEMETRY_TX_EV_ACCEPTED, &key);
        }
        break;

    case CONTRACT_ACK_ST_REJECTED:
        tx->accepting = false;
        switch (ack->reason) {
        case CONTRACT_ACK_RSN_INVALID:
            tx->stats.ack_invalid++;
            /* Nunca será aceito: apaga para não travar a fila. */
            if (journal_ack(tx->journal, &key) == FW_OK) {
                notify(tx, TELEMETRY_TX_EV_REJECTED_INVALID, &key);
            }
            break;
        case CONTRACT_ACK_RSN_CLOCK_SKEW:
            tx->stats.ack_clock_skew++;
            tx->hold_until_ms = now_ms + TELEMETRY_TX_SKEW_HOLD_MS;
            memset(tx->slots, 0, sizeof(tx->slots));
            notify(tx, TELEMETRY_TX_EV_CLOCK_SKEW, &key);
            break;
        case CONTRACT_ACK_RSN_REVOKED:
            tx->stats.ack_revoked++;
            tx->revoked = true;
            notify(tx, TELEMETRY_TX_EV_REVOKED, &key);
            break;
        case CONTRACT_ACK_RSN_UNCLAIMED:
        case CONTRACT_ACK_RSN_UNKNOWN:
        case CONTRACT_ACK_RSN_NONE:
        default:
            if (ack->reason == CONTRACT_ACK_RSN_UNCLAIMED) {
                tx->stats.ack_unclaimed++;
            } else {
                tx->stats.ack_unknown++;
            }
            /* Mantém no journal e tenta mais tarde, com espera crescente.
             * Motivo desconhecido recebe o mesmo tratamento, que é o mais
             * conservador: não perde dado nem inunda o broker. */
            if (now_ms >= tx->hold_until_ms) {
                /* Só a primeira recusa de uma rajada aumenta a espera: a
                 * janela inteira costuma ser recusada de uma vez. */
                tx->hold_until_ms = now_ms + tx->backoff_ms;
                tx->backoff_ms = (tx->backoff_ms >= (TELEMETRY_TX_BACKOFF_MAX_MS / 2U))
                                     ? TELEMETRY_TX_BACKOFF_MAX_MS
                                     : (tx->backoff_ms * 2U);
            }
            memset(tx->slots, 0, sizeof(tx->slots));
            if (ack->reason == CONTRACT_ACK_RSN_UNCLAIMED) {
                notify(tx, TELEMETRY_TX_EV_UNCLAIMED, &key);
            }
            break;
        }
        break;

    default:
        break;
    }
}

void telemetry_tx_on_connect(telemetry_tx_t *tx)
{
    if (tx != NULL) {
        memset(tx->slots, 0, sizeof(tx->slots));
    }
}
