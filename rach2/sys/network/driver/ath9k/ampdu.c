/*
 * ampdu.c: transmit aggregation, A-MPDUs of a TID's frames under the
 * BlockAck session the station agreed with its AP, and each frame's status
 * read from the BlockAck's bitmap.
 *
 * It follows ath9k's xmit.c (Linux v6.16) for one stream at 20 MHz:
 * ath_tx_form_aggr, ath_compute_num_delims, ath_lookup_rate,
 * ath_max_framelen, ath_tx_addto_baw, ath_tx_update_baw and
 * ath_tx_complete_aggr, and main.c's ath9k_parse_mpdudensity. As in
 * mac80211, the station sends the ADDBA request, takes the response and
 * tells the driver through ampdu_action; the driver sends the BlockAckReq
 * itself, as ath_send_bar asks mac80211 to.
 */

#include "ath9k.h"
#include "../../../cmdline.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/kirq.h"
#include "../../../lib/kstring.h"

#define SN_MASK 0xfff

/* ath9k.h's ATH_AGGR_DELIM_SZ and ATH_AGGR_MINPLEN: a delimiter's bytes,
 * and the shortest subframe the chip takes without delimiters after it. */
#define DELIM_SZ 4
#define MIN_PLEN 256

/* The microseconds of an HT20 frame's preamble with one stream, L-STF,
 * L-LTF, L-SIG, HT-SIG, HT-STF and one HT-LTF, and the PLCP's service and
 * tail bits (ath9k's OFDM_PLCP_BITS). */
#define HT_PREAMBLE_US 36
#define PLCP_BITS      22

/* A queue's TXOP in microseconds, mac80211's defaults: 0 for BE and BK,
 * which ath9k takes as 4 ms. */
#define TXOP_VO      1504
#define TXOP_VI      3008
#define TXOP_DEFAULT 4096

/* The AP's A-MPDU length and MPDU spacing until its HT Capabilities are
 * known: the least every HT station takes, a factor of 0, and the widest
 * spacing, 16 microseconds. */
#define CAUTIOUS_LEN     8191
#define CAUTIOUS_DENSITY 16

/* A BlockAckReq (802.11-2020 9.3.1.7): frame control, duration, RA, TA,
 * BAR control and the starting sequence control. Its control asks for a
 * compressed BlockAck of one TID. */
#define BAR_LEN        20
#define BAR_COMPRESSED 0x0004

/* HT20's data bits per OFDM symbol of MCS 0 to 7, one stream
 * (ath9k's bits_per_symbol). */
static const uint16_t ht20_bits[8] = {26, 52, 78, 104, 156, 208, 234, 260};

/** Whether two n-byte strings are equal. */
static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** How far sequence number `b` is past `a`, modulo 4096. */
static unsigned sn_sub(unsigned b, unsigned a)
{
    return (b - a) & SN_MASK;
}

/** Whether sequence number `a` comes before `b`, modulo 4096. */
static int sn_less(unsigned a, unsigned b)
{
    return sn_sub(a, b) > (SN_MASK + 1) / 2;
}

/** The slot `i` places past queue `q`'s head. */
static struct ath9k_tx_slot *slot_at(struct ath9k_txq *q, unsigned i)
{
    return &q->slot[(q->head + i) % ATH9K_TX_RING];
}

/**
 * The session slot `s` is sent under, or NULL: a QoS data frame, with an
 * acknowledgement, of a TID whose session is active, to its peer, while
 * aggregation is on.
 */
static struct ath9k_tid *session_of(struct ath9k *ah, const struct ath9k_tx_slot *s)
{
    struct ath9k_tid *t;

    if (s->own || (ah->options & ATH9K_OPT_NO_AMPDU) || s->info.tid >= 8 ||
        (s->info.flags & WLAN_TX_NO_ACK))
        return 0;
    if ((s->fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) !=
        (IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA))
        return 0;
    t = &ah->ampdu.tid[s->info.tid];
    if (!t->active || !same(s->buf + 4, ah->ampdu.peer, 6))
        return 0;
    return t;
}

/** Owe the recipient a BlockAckReq once the window has passed `seq`. */
static void owe_bar(struct ath9k_tid *t, unsigned seq)
{
    seq &= SN_MASK;
    if (!t->bar_owed || sn_less(t->bar_seq, seq))
        t->bar_seq = (uint16_t)seq;
    t->bar_owed = 1;
}

/**
 * Let slot `s` into its window, as ath_tx_addto_baw does on a frame's
 * first push. Returns 1 when it is in the window, now or from before; 0
 * when it lies past the window and must wait for the window to move; -1
 * when its number comes before every number the window has taken, so it
 * goes alone outside the session. A number skipped since the last one
 * taken, a frame the radio refused or one dropped by a reset, is done at
 * once and owes the recipient a BlockAckReq past it; an empty window moves
 * up to the frame.
 */
static int admit(struct ath9k_tid *t, struct ath9k_tx_slot *s)
{
    if (s->ba && s->gen == t->gen)
        return 1;
    s->ba = 0;
    if (sn_less(s->seq, t->seq_next))
        return -1;
    if (s->seq != t->seq_next) {
        owe_bar(t, s->seq);
        if (!t->out)
            t->seq_start = s->seq;
    }
    if (sn_sub(s->seq, t->seq_start) >= t->window)
        return 0;
    t->out |= (uint64_t)1 << sn_sub(s->seq, t->seq_start);
    t->seq_next = (uint16_t)((s->seq + 1) & SN_MASK);
    s->ba = 1;
    s->gen = t->gen;
    s->tries = 0;
    return 1;
}

/**
 * Mark `seq` done and move the window's start past every number done, as
 * ath_tx_update_baw does.
 */
static void done(struct ath9k_tid *t, unsigned seq)
{
    unsigned i = sn_sub(seq, t->seq_start);

    if (i >= ATH9K_BA_WINDOW)
        return;
    t->out &= ~((uint64_t)1 << i);
    while (t->seq_start != t->seq_next && !(t->out & 1)) {
        t->out >>= 1;
        t->seq_start = (uint16_t)((t->seq_start + 1) & SN_MASK);
    }
}

/**
 * The most bytes an HT20 frame at series `r` carries in `usec`
 * microseconds, as ath_max_framelen computes them.
 */
static unsigned max_framelen(unsigned usec, const struct wlan_tx_rate *r)
{
    unsigned symbols, bits;

    usec -= HT_PREAMBLE_US;
    symbols = (r->flags & WLAN_RATE_SHORT_GI) ? usec * 5 / 18 : usec >> 2;
    bits = symbols * ht20_bits[r->index & 7];
    bits = bits > PLCP_BITS ? bits - PLCP_BITS : 0;
    return bits / 8 > 65532 ? 65532 : bits / 8;
}

/**
 * The longest aggregate queue `q` may send at `rates`, as ath_lookup_rate
 * finds it: what the slowest series sends in the queue's TXOP, at most the
 * chip's and the AP's limits, and 0 when the first series is not an MCS.
 * The series from the first that is not an MCS onward are cut from
 * `rates`, since an aggregate cannot fall back to a legacy rate.
 */
static unsigned aggr_limit(struct ath9k *ah, const struct ath9k_txq *q,
                           struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES])
{
    unsigned i, txop = TXOP_DEFAULT, limit = ATH9K_AMPDU_LIMIT_MAX, len;

    if (q == &ah->tx.q[WLAN_AC_VO])
        txop = TXOP_VO;
    else if (q == &ah->tx.q[WLAN_AC_VI])
        txop = TXOP_VI;
    for (i = 0; i < WLAN_TX_RATE_SERIES; i++) {
        if (rates[i].index < 0 || rates[i].count == 0)
            break;
        if (!(rates[i].flags & WLAN_RATE_MCS) || rates[i].index > 7) {
            if (i == 0)
                return 0;
            for (; i < WLAN_TX_RATE_SERIES; i++) {
                rates[i].index = -1;
                rates[i].count = 0;
            }
            break;
        }
        len = max_framelen(txop, &rates[i]);
        if (len < limit)
            limit = len;
    }
    return ah->ampdu.max_len < limit ? ah->ampdu.max_len : limit;
}

/**
 * The delimiters after a subframe of `framelen` bytes in an aggregate sent
 * at `r`, as ath_compute_num_delims counts them for an AR9003 with EDMA:
 * enough for the chip's shortest subframe, and enough that the next
 * subframe starts no sooner than the AP's MPDU spacing at `r`.
 */
static unsigned delimiters(const struct ath9k *ah, const struct wlan_tx_rate *r, unsigned framelen)
{
    unsigned n = framelen >= MIN_PLEN ? 0 : (MIN_PLEN - framelen + DELIM_SZ - 1) / DELIM_SZ;
    unsigned density = ah->ampdu.density, symbols, minlen;

    if (density == 0)
        return n;
    symbols = (r->flags & WLAN_RATE_SHORT_GI) ? (density * 5 - 4) / 18 : density >> 2;
    if (symbols == 0)
        symbols = 1;
    minlen = symbols * ht20_bits[r->index & 7] / 8;
    if (framelen < minlen && (minlen - framelen) / DELIM_SZ > n)
        n = (minlen - framelen) / DELIM_SZ;
    return n;
}

/** The bytes that pad `len` to a multiple of 4. */
static unsigned pad4(unsigned len)
{
    return (4 - (len & 3)) & 3;
}

/** The chains of BlockAck sessions queue `q`'s FIFO holds. */
static unsigned session_depth(const struct ath9k_txq *q)
{
    unsigned k, depth = 0;

    for (k = 0; k < q->chains; k++)
        depth += q->chain[(q->chain_head + k) % ATH9K_TX_FIFO_DEPTH].ba;
    return depth;
}

/**
 * Choose the next push of queue `q`, as ath_tx_form_aggr does: a frame
 * outside any session goes alone; a frame of a session waits, holding the
 * queue back, while the FIFO holds ATH9K_AMPDU_DEPTH chains of sessions, so
 * that the frames behind it gather into an aggregate, as ath_txq_schedule
 * has them, or while its window has no room; otherwise the aggregate takes
 * the frames that follow it while they are of its session, start at an
 * MCS, fit in its window and its length limit, and number at most half
 * the window. Kept frames come first in the ring, so a retry goes before
 * new frames.
 */
unsigned ath9k_ampdu_form(struct ath9k *ah, struct ath9k_txq *q, unsigned *aggr_len,
                          struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES])
{
    struct ath9k_tx_slot *s = slot_at(q, q->queued);
    struct ath9k_tid *t = session_of(ah, s);
    struct wlan_tx_rate cut[WLAN_TX_RATE_SERIES];
    unsigned n, half, limit, al, delta, bpad, avail = q->count - q->queued;
    int in;

    k_memcpy(rates, s->info.rates, sizeof(s->info.rates));
    *aggr_len = 0;
    s->ndelim = 0;
    if (!t) {
        s->ba = 0;
        return 1;
    }
    if (session_depth(q) >= ATH9K_AMPDU_DEPTH)
        return 0;
    in = admit(t, s);
    if (in <= 0)
        return in < 0 ? 1 : 0;
    k_memcpy(cut, rates, sizeof(cut));
    limit = aggr_limit(ah, q, cut);
    if (limit == 0)
        return 1;
    half = t->window / 2;
    if (half > 32)
        half = 32;
    delta = DELIM_SZ + s->framelen;
    al = delta;
    s->ndelim = (uint8_t)delimiters(ah, &cut[0], s->framelen);
    bpad = pad4(delta) + s->ndelim * DELIM_SZ;
    for (n = 1; n < avail && n < half; n++) {
        struct ath9k_tx_slot *next = slot_at(q, q->queued + n);

        if (session_of(ah, next) != t || !(next->info.rates[0].flags & WLAN_RATE_MCS))
            break;
        delta = DELIM_SZ + next->framelen;
        if (limit < al + bpad + delta || admit(t, next) != 1)
            break;
        al += bpad + delta;
        next->ndelim = (uint8_t)delimiters(ah, &cut[0], next->framelen);
        bpad = pad4(delta) + next->ndelim * DELIM_SZ;
    }
    if (n == 1)
        return 1;
    k_memcpy(rates, cut, sizeof(cut));
    *aggr_len = al;
    ah->ampdu.aggregates++;
    ah->ampdu.subframes += n;
    return n;
}

int ath9k_ampdu_owns(const struct ath9k *ah, const struct ath9k_txq *q)
{
    (void)ah;
    return q->slot[q->head].ba;
}

/**
 * Finish the oldest chain of `q`, as ath_tx_complete_aggr does. The chain
 * succeeded when ath9k_tx_ok says so; an aggregate that succeeded carries
 * the BlockAck's bitmap from its starting number, and one with no bitmap
 * asks for a reset, as ath9k does for a station. A frame is acknowledged
 * by its bit, or, sent alone, by the chain's success. A frame of its
 * session that was not stays in the ring for another push, its Retry bit
 * set, until ATH9K_BA_MAX_TRIES attempts; then it is given up, and the
 * recipient is owed a BlockAckReq past it. A frame of a session that has
 * ended goes nowhere again. The first frame finished carries the chain's
 * tries to rate control, as ath9k's rc_update does; the rest report none.
 */
void ath9k_ampdu_status(struct ath9k *ah, struct ath9k_txq *q, const struct ar9003_txs *ts)
{
    const struct ath9k_tx_chain *c = &q->chain[q->chain_head];
    unsigned n = c->n, i, final, attempts, seq_st;
    uint32_t keep = 0, ba[2] = {0, 0};
    int ok, has_ba, rates = ATH9K_TX_RATES;

    ok = ath9k_tx_ok(ts, 1);
    has_ba = c->aggr && ok && (ts->status2 & AR_TxBaStatus);
    seq_st = MS(ts->status8, AR_SeqNum);
    if (has_ba) {
        ba[0] = ts->status5;
        ba[1] = ts->status6;
    } else if (c->aggr && ok) {
        ath9k_reset_schedule(ah, ATH9K_RESET_TX_ERROR);
    }
    final = MS(ts->status8, AR_FinalTxIdx);
    attempts = MS(ts->status3, AR_DataFailCnt) + 1;
    for (i = 0; i < final && i < WLAN_TX_RATE_SERIES; i++)
        attempts += c->rates[i].count;

    for (i = 0; i < n; i++) {
        struct ath9k_tx_slot *s = slot_at(q, i);
        struct ath9k_tid *t = &ah->ampdu.tid[s->info.tid & 7];
        unsigned bit = sn_sub(s->seq, seq_st);
        int tracked = s->ba && t->active && s->gen == t->gen, acked;

        if (c->aggr)
            acked = has_ba && bit < ATH9K_BA_WINDOW && ((ba[bit >> 5] >> (bit & 31)) & 1);
        else
            acked = ok;
        if (!acked && tracked && s->tries < ATH9K_BA_MAX_TRIES) {
            s->tries = (uint8_t)(s->tries + attempts > 255 ? 255 : s->tries + attempts);
            s->fc |= IEEE80211_FCTL_RETRY;
            s->buf[1] |= IEEE80211_FCTL_RETRY >> 8;
            ah->ampdu.sw_retries++;
            keep |= (uint32_t)1 << i;
            continue;
        }
        if (tracked) {
            done(t, s->seq);
            if (!acked)
                owe_bar(t, s->seq + 1);
        }
        ath9k_tx_report(ah, s, c, ts, (acked ? ATH9K_TX_ACKED : 0) | rates);
        rates = 0;
    }
    ath9k_tx_retire(ah, q, keep);
}

void ath9k_ampdu_drain(struct ath9k *ah)
{
    unsigned i;

    for (i = 0; i < 8; i++) {
        struct ath9k_tid *t = &ah->ampdu.tid[i];

        if (!t->active || !t->out)
            continue;
        t->out = 0;
        t->seq_start = t->seq_next;
        owe_bar(t, t->seq_next);
    }
}

/**
 * Queue a BlockAckReq for each session the window has carried past the
 * number it owes one for, as ath_send_bar does: its starting number is the
 * window's start, before which every frame is done, so it skips the frames
 * given up and passes over none still to be sent. It goes on its TID's
 * queue behind every frame there, at the band's lowest rate. One that finds
 * no room stays owed.
 */
void ath9k_ampdu_bar(struct ath9k *ah)
{
    static const uint8_t tid_ac[8] = {WLAN_AC_BE, WLAN_AC_BK, WLAN_AC_BK, WLAN_AC_BE,
                                      WLAN_AC_VI, WLAN_AC_VI, WLAN_AC_VO, WLAN_AC_VO};
    uint8_t f[BAR_LEN];
    struct wlan_tx_info info;
    unsigned i, ctl;

    for (i = 0; i < 8; i++) {
        struct ath9k_tid *t = &ah->ampdu.tid[i];

        if (!t->active || !t->bar_owed || sn_less(t->seq_start, t->bar_seq))
            continue;
        k_memset(f, 0, sizeof(f));
        f[0] = (uint8_t)(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_BACK_REQ);
        k_memcpy(f + 4, ah->ampdu.peer, 6);
        k_memcpy(f + 10, ah->radio.mac, 6);
        ctl = BAR_COMPRESSED | i << 12;
        f[16] = (uint8_t)ctl;
        f[17] = (uint8_t)(ctl >> 8);
        f[18] = (uint8_t)(t->seq_start << 4);
        f[19] = (uint8_t)(t->seq_start >> 4);
        k_memset(&info, 0, sizeof(info));
        info.rates[0].index = 0;
        info.rates[0].count = ATH9K_TX_MAX_TRIES;
        info.rates[1].index = info.rates[2].index = info.rates[3].index = -1;
        info.ac = tid_ac[i];
        info.tid = (uint8_t)i;
        info.hw_key = -1;
        t->bar_owed = 0;
        if (ath9k_tx_own(ah, f, sizeof(f), &info) != 0) {
            t->bar_owed = 1;
            continue;
        }
        ah->ampdu.bars++;
    }
}

/** ath9k_parse_mpdudensity: the microseconds of an HT Capabilities
 * spacing code, 1/4 to 1 microsecond taken as 1. */
static unsigned density_us(unsigned code)
{
    static const uint8_t us[8] = {0, 1, 1, 1, 2, 4, 8, 16};

    return us[code & 7];
}

void ath9k_ampdu_sta(struct ath9k *ah, const struct wlan_bss *bss)
{
    unsigned at = 0, params;
    uint32_t flags;

    while (bss && at + 2 <= bss->ies_len && at + 2 + bss->ies[at + 1] <= bss->ies_len) {
        if (bss->ies[at] == WLAN_EID_HT_CAPABILITY && bss->ies[at + 1] >= 3) {
            params = bss->ies[at + 4];
            flags = kirq_save();
            ah->ampdu.max_len = (uint16_t)((1u << (13 + (params & 3))) - 1);
            ah->ampdu.density = (uint8_t)density_us(params >> 2);
            kirq_restore(flags);
            return;
        }
        at += 2 + bss->ies[at + 1];
    }
    flags = kirq_save();
    ah->ampdu.max_len = CAUTIOUS_LEN;
    ah->ampdu.density = CAUTIOUS_DENSITY;
    kirq_restore(flags);
}

/**
 * Start or stop a transmit session, as ath_tx_aggr_start and
 * ath_tx_aggr_stop do. A start opens the TID's window at `ssn`, of
 * `window` frames up to ATH9K_BA_WINDOW, as a new session that frames of
 * an older one are not in; frames queued before it with lower numbers go
 * alone. A stop leaves the frames already queued to go alone, as
 * single frames, and owes no BlockAckReq.
 */
int ath9k_ampdu_action(struct ath9k *ah, enum wlan_ampdu_action action, const uint8_t peer[6],
                       unsigned tid, unsigned ssn, unsigned window)
{
    struct ath9k_tid *t;
    uint32_t flags;

    if (kcmdline_has("rach2.ath9k_noampdu"))
        ah->options |= ATH9K_OPT_NO_AMPDU;
    if (action != WLAN_AMPDU_TX_START && action != WLAN_AMPDU_TX_STOP)
        return -EOPNOTSUPP;
    if (tid >= 8)
        return -EINVAL;
    t = &ah->ampdu.tid[tid];
    flags = kirq_save();
    if (action == WLAN_AMPDU_TX_STOP) {
        t->active = 0;
        t->bar_owed = 0;
        kirq_restore(flags);
        return 0;
    }
    if ((ah->options & ATH9K_OPT_NO_AMPDU) || !peer || window == 0) {
        kirq_restore(flags);
        return (ah->options & ATH9K_OPT_NO_AMPDU) ? -EOPNOTSUPP : -EINVAL;
    }
    k_memcpy(ah->ampdu.peer, peer, 6);
    if (ah->ampdu.max_len == 0) {
        ah->ampdu.max_len = CAUTIOUS_LEN;
        ah->ampdu.density = CAUTIOUS_DENSITY;
    }
    t->gen++;
    t->active = 1;
    t->window = (uint16_t)(window > ATH9K_BA_WINDOW ? ATH9K_BA_WINDOW : window);
    t->seq_start = t->seq_next = (uint16_t)(ssn & SN_MASK);
    t->out = 0;
    t->bar_owed = 0;
    kirq_restore(flags);
    return 0;
}
