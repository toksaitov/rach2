/*
 * xmit.c: the AR9003's transmit, a queue per access category whose frames
 * the chip takes by descriptor and reports in one status ring.
 *
 * It follows ath9k's xmit.c and ar9003_mac.c's ar9003_set_txdesc and
 * ar9003_hw_proc_txdesc (Linux v6.16), for single frames: each frame is one
 * descriptor of one buffer, and each push to a queue's FIFO is one frame.
 * Aggregates are ampdu.c's.
 */

#include "ath9k.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/kirq.h"
#include "../../../lib/kstring.h"

/* ath9k's hw.h and mac.h: the power a descriptor asks for when per-frame
 * power control is off, as it is, which leaves the rate's own limit; the
 * key types of AR_EncrType; and the frame types of AR_FrameType. */
#define MAX_RATE_POWER      63
#define KEY_TYPE_CLEAR      0
#define KEY_TYPE_AES        2
#define KEY_TYPE_TKIP       3
#define PKT_TYPE_NORMAL     0
#define PKT_TYPE_ATIM       1
#define PKT_TYPE_PSPOLL     2
#define PKT_TYPE_BEACON     3
#define PKT_TYPE_PROBE_RESP 4

#define FCS_LEN 4

/* The chip's code of each legacy rate, in 100 kbit/s, as ath9k's rate
 * table gives them; a CCK rate but 1 Mbit/s takes ATH9K_RATE_CCK_SP for a
 * short preamble. An MCS is ATH9K_RATE_MCS with its index. */
#define ATH9K_RATE_CCK_SP 0x04
#define ATH9K_RATE_MCS    0x80

static const struct {
    uint16_t rate; /* 100 kbit/s */
    uint8_t code;
    uint8_t cck;
} rate_codes[] = {{10, 0x1b, 1},  {20, 0x1a, 1},  {55, 0x19, 1},  {110, 0x18, 1},
                  {60, 0x0b, 0},  {90, 0x0f, 0},  {120, 0x0a, 0}, {180, 0x0e, 0},
                  {240, 0x09, 0}, {360, 0x0d, 0}, {480, 0x08, 0}, {540, 0x0c, 0}};

#define N_RATE_CODES (sizeof(rate_codes) / sizeof(rate_codes[0]))

/* HT20's data bits per OFDM symbol of MCS 0 to 7, one stream
 * (ath9k's bits_per_symbol). */
static const uint16_t ht20_bits[8] = {26, 52, 78, 104, 156, 208, 234, 260};

/**
 * The band index of the chip's rate code `code`, or its MCS index with
 * WLAN_RATE_MCS in *flags; WLAN_RATE_SHORT_PREAMBLE is added for a CCK
 * rate sent with one. Returns -1 for a code the band does not have.
 */
int ath9k_rate_index(const struct ath9k *ah, unsigned code, uint8_t *flags)
{
    unsigned i, j;

    *flags = 0;
    if (code & ATH9K_RATE_MCS) {
        *flags = WLAN_RATE_MCS;
        return (int)(code & 0x7f);
    }
    for (i = 0; i < N_RATE_CODES; i++) {
        unsigned c = rate_codes[i].code;

        if (code != c &&
            !(rate_codes[i].cck && rate_codes[i].rate != 10 && code == (c | ATH9K_RATE_CCK_SP)))
            continue;
        if (code != c)
            *flags = WLAN_RATE_SHORT_PREAMBLE;
        for (j = 0; j < ah->radio.band.n_rates; j++)
            if (ah->radio.band.rates[j] == rate_codes[i].rate)
                return (int)j;
        return -1;
    }
    return -1;
}

/** The entry of rate_codes for a series, or -1 for an MCS or an index
 * the band does not have. */
static int legacy_entry(const struct ath9k *ah, const struct wlan_tx_rate *r)
{
    unsigned i, rate;

    if ((r->flags & WLAN_RATE_MCS) || r->index < 0 || (unsigned)r->index >= ah->radio.band.n_rates)
        return -1;
    rate = ah->radio.band.rates[r->index];
    for (i = 0; i < N_RATE_CODES; i++)
        if (rate_codes[i].rate == rate)
            return (int)i;
    return -1;
}

/**
 * The air time of `len` bytes at series `r`, with the SIFS and the
 * preamble, in microseconds, as ath9k_hw_computetxtime gives it for a
 * legacy rate and ath_pkt_duration for HT20. 0 for an unknown rate.
 */
static unsigned air_time(const struct ath9k *ah, const struct wlan_tx_rate *r, unsigned len)
{
    unsigned bits, nsym, kbps;
    int e;

    if (r->flags & WLAN_RATE_MCS) {
        unsigned per = ht20_bits[r->index & 7];

        nsym = ((len << 3) + 22 + per - 1) / per;
        /* L-STF, L-LTF, L-SIG, HT-SIG, HT-STF and one HT-LTF. */
        return ((r->flags & WLAN_RATE_SHORT_GI) ? (nsym * 18 + 4) / 5 : nsym * 4) + 36;
    }
    e = legacy_entry(ah, r);
    if (e < 0)
        return 0;
    kbps = rate_codes[e].rate * 100u;
    if (rate_codes[e].cck) {
        unsigned phy = (r->flags & WLAN_RATE_SHORT_PREAMBLE) ? 96 : 192;

        return 10 + phy + (len << 3) * 1000 / kbps;
    }
    bits = kbps * 4 / 1000;
    nsym = (22 + (len << 3) + bits - 1) / bits;
    return 16 + 20 + nsym * 4;
}

/** The chip's rate code of series `r`, or 0 for one it cannot send. */
static unsigned rate_code(const struct ath9k *ah, const struct wlan_tx_rate *r)
{
    int e;

    if (r->flags & WLAN_RATE_MCS)
        return r->index >= 0 && r->index < 8 ? ATH9K_RATE_MCS | (unsigned)r->index : 0;
    e = legacy_entry(ah, r);
    if (e < 0)
        return 0;
    if (rate_codes[e].cck && rate_codes[e].rate != 10 && (r->flags & WLAN_RATE_SHORT_PREAMBLE))
        return rate_codes[e].code | ATH9K_RATE_CCK_SP;
    return rate_codes[e].code;
}

/**
 * Allocate the status ring and, for each queue's ring of frames, the
 * descriptors, 32 to a page, and the frames' buffers, two to a page.
 * Returns 0 or -ENOMEM, having given back what it took.
 */
int ath9k_tx_init(struct ath9k *ah)
{
    unsigned g, total = ATH9K_TX_QUEUES * ATH9K_TX_RING;
    unsigned per_desc = ATH9K_DMA_PAGE / sizeof(struct ar9003_txc);
    unsigned per_buf = ATH9K_DMA_PAGE / ATH9K_TX_BUF_LEN;
    uint8_t *dpage = 0, *bpage = 0;
    uint32_t dbus = 0, bbus = 0;

    ah->tx.status = ath9k_dma_alloc(ah, &ah->tx.status_bus);
    if (!ah->tx.status)
        return -ENOMEM;
    for (g = 0; g < total; g++) {
        struct ath9k_tx_slot *s = &ah->tx.q[g / ATH9K_TX_RING].slot[g % ATH9K_TX_RING];

        if (g % per_desc == 0 && !(dpage = ath9k_dma_alloc(ah, &dbus)))
            goto fail;
        if (g % per_buf == 0 && !(bpage = ath9k_dma_alloc(ah, &bbus)))
            goto fail;
        s->desc = (struct ar9003_txc *)(void *)(dpage + (g % per_desc) * sizeof(struct ar9003_txc));
        s->desc_bus = dbus + (g % per_desc) * sizeof(struct ar9003_txc);
        s->buf = bpage + (g % per_buf) * ATH9K_TX_BUF_LEN;
        s->buf_bus = bbus + (g % per_buf) * ATH9K_TX_BUF_LEN;
    }
    return 0;
fail:
    /* The descriptor page taken for the slot whose buffer failed. */
    if (g % per_desc == 0 && dpage)
        ath9k_dma_free(ah, dpage);
    for (total = g, g = 0; g < total; g++) {
        struct ath9k_tx_slot *s = &ah->tx.q[g / ATH9K_TX_RING].slot[g % ATH9K_TX_RING];

        if (g % per_desc == 0)
            ath9k_dma_free(ah, s->desc);
        if (g % per_buf == 0)
            ath9k_dma_free(ah, s->buf);
        s->desc = 0;
        s->buf = 0;
    }
    ath9k_dma_free(ah, ah->tx.status);
    ah->tx.status = 0;
    return -ENOMEM;
}

/** ath9k's AR_FrameType for a frame whose frame control is `fc`. */
static unsigned frame_type(unsigned fc)
{
    unsigned type = fc & IEEE80211_FCTL_FTYPE, stype = fc & IEEE80211_FCTL_STYPE;

    if (type == IEEE80211_FTYPE_CTL && stype == IEEE80211_STYPE_PSPOLL)
        return PKT_TYPE_PSPOLL;
    if (type != IEEE80211_FTYPE_MGMT)
        return PKT_TYPE_NORMAL;
    switch (stype) {
    case IEEE80211_STYPE_BEACON:
        return PKT_TYPE_BEACON;
    case IEEE80211_STYPE_PROBE_RESP:
        return PKT_TYPE_PROBE_RESP;
    case 0x0090: /* ATIM */
        return PKT_TYPE_ATIM;
    default:
        return PKT_TYPE_NORMAL;
    }
}

/**
 * Fill slot `s`'s descriptor for its frame of `buflen` bytes in the
 * buffer, `framelen` on the air with the FCS and what the key cache adds,
 * as ar9003_set_txdesc does for a frame that is both first and last: one
 * buffer, four rate series, the key's entry and type, and the checksum of
 * the descriptor's pointers.
 */
static void fill_desc(struct ath9k *ah, const struct ath9k_txq *q, struct ath9k_tx_slot *s,
                      unsigned buflen, unsigned framelen, unsigned fc, int keyix, unsigned keytype)
{
    struct ar9003_txc *d = s->desc;
    const struct wlan_tx_rate *r = s->info.rates;
    unsigned tries[WLAN_TX_RATE_SERIES], codes[WLAN_TX_RATE_SERIES], dur[WLAN_TX_RATE_SERIES];
    unsigned i, chains = ah->hw.chip ? ah->hw.chip->chainmask : 1;
    uint32_t sum, ctl18 = 0, rts_flags = 0;
    int ended = 0;

    for (i = 0; i < WLAN_TX_RATE_SERIES; i++) {
        unsigned code = ended ? 0 : rate_code(ah, &r[i]);

        if (ended || r[i].index < 0 || r[i].count == 0 || code == 0) {
            ended = 1;
            tries[i] = codes[i] = dur[i] = 0;
            continue;
        }
        tries[i] = r[i].count > 15 ? 15 : r[i].count;
        codes[i] = code;
        dur[i] = air_time(ah, &r[i], framelen);
        if (r[i].flags & WLAN_RATE_RTS_CTS)
            rts_flags |= 1u << i;
    }
    /* Each series' rate flags are 5 bits apart, and its chains with them. */
    for (i = 0; i < WLAN_TX_RATE_SERIES; i++)
        if (tries[i])
            ctl18 |=
                (((r[i].flags & WLAN_RATE_SHORT_GI) && (r[i].flags & WLAN_RATE_MCS) ? AR_GI0 : 0) |
                 SM(chains, AR_ChainSel0))
                << (5 * i);

    k_memset(d, 0, sizeof(*d));
    d->info = SM(ATH9K_VENDOR_ID, AR_DescId) | AR_TxRxDesc | AR_CtrlStat |
              SM(q->qnum, AR_TxQcuNum) |
              ((ah->hw.chip && (ah->hw.chip->caps & ATH9K_CHIP_JUPITER)) ? 0x18 : 0x17);
    d->link = 0;
    d->data0 = s->buf_bus;
    d->ctl3 = SM(buflen, AR_BufLen);
    sum = d->info + d->link + d->data0 + d->ctl3;
    d->ctl10 = ((sum & 0xffff) + (sum >> 16)) & AR_TxPtrChkSum;
    d->ctl11 = (framelen & AR_FrameLen) | SM(MAX_RATE_POWER, AR_XmitPower0) |
               (keyix >= 0 ? AR_DestIdxValid : 0) | AR_ClrDestMask |
               ((rts_flags & 1) ? AR_RTSEnable : 0);
    d->ctl12 = (keyix >= 0 ? SM(keyix, AR_DestIdx) : 0) | SM(frame_type(fc), AR_FrameType) |
               ((s->info.flags & WLAN_TX_NO_ACK) ? AR_NoAck : 0);
    d->ctl13 = SM(tries[0], AR_XmitDataTries0) | SM(tries[1], AR_XmitDataTries1) |
               SM(tries[2], AR_XmitDataTries2) | SM(tries[3], AR_XmitDataTries3) |
               (frame_type(fc) == PKT_TYPE_PSPOLL ? 0 : AR_DurUpdateEna);
    d->ctl14 = SM(codes[0], AR_XmitRate0) | SM(codes[1], AR_XmitRate1) |
               SM(codes[2], AR_XmitRate2) | SM(codes[3], AR_XmitRate3);
    d->ctl15 = SM(dur[0], AR_PacketDur0) | ((rts_flags & 1) ? AR_RTSCTSQual0 : 0) |
               SM(dur[1], AR_PacketDur1) | ((rts_flags & 2) ? AR_RTSCTSQual1 : 0);
    d->ctl16 = SM(dur[2], AR_PacketDur2) | ((rts_flags & 4) ? AR_RTSCTSQual2 : 0) |
               SM(dur[3], AR_PacketDur3) | ((rts_flags & 8) ? AR_RTSCTSQual3 : 0);
    d->ctl17 = SM(keytype, AR_EncrType);
    d->ctl18 = ctl18 | SM(rate_codes[0].code, AR_RTSCTSRate);
    d->ctl19 = AR_Not_Sounding;
    d->ctl20 = SM(MAX_RATE_POWER, AR_XmitPower1);
    d->ctl21 = SM(MAX_RATE_POWER, AR_XmitPower2);
    d->ctl22 = SM(MAX_RATE_POWER, AR_XmitPower3);
}

/** Push queue `q`'s accepted frames to its FIFO while it has room. */
static void kick(struct ath9k *ah, struct ath9k_txq *q)
{
    while (q->queued < q->count && q->queued < ATH9K_TX_FIFO_DEPTH) {
        struct ath9k_tx_slot *s = &q->slot[(q->head + q->queued) % ATH9K_TX_RING];

        k_barrier();
        ath9k_mac_tx_push(ah, q->qnum, s->desc_bus);
        q->queued++;
    }
}

/**
 * Accept a frame for info->ac's queue, as ath_tx_start does for a single
 * frame: copy it with ath9k's padding after a header that is not a
 * multiple of 4 bytes, leave out the MIC or ICV space the station left
 * for the key cache, which the chip appends, fill its descriptor and push
 * it when its FIFO has room. Returns 0; -EINVAL for a frame too short or
 * too long, a queue that is not one, or a key slot not in use; -ENOBUFS
 * when the queue is full or a reset runs; -ENETDOWN when the queues are
 * not set up.
 */
int ath9k_tx(struct ath9k *ah, const uint8_t *frame, unsigned len, const struct wlan_tx_info *info)
{
    struct ath9k_txq *q;
    struct ath9k_tx_slot *s;
    unsigned fc, hdrlen, pad = 0, trailer = 0, keytype = KEY_TYPE_CLEAR, buflen;
    int keyix = -1;
    uint32_t flags;

    if (!ah->tx.running)
        return -ENETDOWN;
    if (len < 10 || info->ac >= ATH9K_TX_QUEUES)
        return -EINVAL;
    fc = (unsigned)frame[0] | (unsigned)frame[1] << 8;
    hdrlen = ath9k_header_len(fc);
    if ((hdrlen & 3) && len > hdrlen)
        pad = 4 - (hdrlen & 3);
    if (info->flags & WLAN_TX_HW_KEY) {
        const struct ath9k_key *k;

        if (info->hw_key < 0 || info->hw_key >= AR_KEY_CACHE_SIZE)
            return -EINVAL;
        k = &ah->keys.entry[info->hw_key];
        if (k->used != ATH9K_KEY_USED)
            return -EINVAL;
        keyix = info->hw_key;
        if (k->type == AR_KEYTABLE_TYPE_TKIP) {
            keytype = KEY_TYPE_TKIP;
            trailer = 4; /* the ICV */
        } else {
            keytype = KEY_TYPE_AES;
            trailer = 8; /* CCMP's MIC */
        }
    }
    if (len < hdrlen + trailer || len - trailer + pad > ATH9K_TX_BUF_LEN)
        return -EINVAL;
    buflen = len - trailer + pad;

    flags = kirq_save();
    q = &ah->tx.q[info->ac];
    if (ah->recover.resetting || q->count == ATH9K_TX_RING) {
        kirq_restore(flags);
        return -ENOBUFS;
    }
    s = &q->slot[(q->head + q->count) % ATH9K_TX_RING];
    if (pad) {
        k_memcpy(s->buf, frame, hdrlen);
        k_memset(s->buf + hdrlen, 0, pad);
        k_memcpy(s->buf + hdrlen + pad, frame + hdrlen, len - trailer - hdrlen);
    } else {
        k_memcpy(s->buf, frame, len - trailer);
    }
    s->len = len;
    s->info = *info;
    fill_desc(ah, q, s, buflen, len + FCS_LEN, fc, keyix, keytype);
    q->count++;
    kick(ah, q);
    kirq_restore(flags);
    return 0;
}

/**
 * Report queue `q`'s oldest pushed frame finished with status `ts`, as
 * ath_tx_rc_status reads it: the series before the last one tried used
 * all their tries, the last its failures and the one that ended it, and
 * the frame was acknowledged when no error ended it.
 */
static void complete(struct ath9k *ah, struct ath9k_txq *q, const struct ar9003_txs *ts)
{
    struct ath9k_tx_slot *s = &q->slot[q->head];
    struct wlan_tx_status st;
    unsigned final = MS(ts->status8, AR_FinalTxIdx), i, tries = 0;
    int failed =
        (ts->status3 & (AR_ExcessiveRetries | AR_Filtered | AR_FIFOUnderrun | AR_TxTimerExpired)) ||
        (ts->status8 & AR_TxOpExceeded);

    k_memset(&st, 0, sizeof(st));
    st.cookie = s->info.cookie;
    for (i = 0; i < WLAN_TX_RATE_SERIES; i++) {
        st.rates[i] = s->info.rates[i];
        if (i == final)
            st.rates[i].count = (uint8_t)(MS(ts->status3, AR_DataFailCnt) + 1);
        else if (i > final) {
            st.rates[i].index = -1;
            st.rates[i].count = 0;
        }
        tries += st.rates[i].count;
    }
    if (!failed) {
        int rssi = (int8_t)MS(ts->status7, AR_TxRSSICombined);

        st.flags = WLAN_TX_ACKED;
        if (!(s->info.flags & WLAN_TX_NO_ACK))
            st.ack_signal = (int16_t)((ah->cal.noise_floor ? ah->cal.noise_floor : -95) + rssi);
    } else {
        ah->stats.tx_failed++;
        if (ts->status3 & AR_Filtered)
            st.flags = WLAN_TX_FILTERED;
    }
    if (ts->status3 & (AR_FIFOUnderrun | AR_TxDataUnderrun | AR_TxDelimUnderrun))
        ah->stats.tx_underrun++;
    ah->stats.tx_frames++;
    if (tries > 1)
        ah->stats.tx_retries += tries - 1;
    q->head = (q->head + 1) % ATH9K_TX_RING;
    q->count--;
    q->queued--;
    q->inflight = 0;
    wlan_radio_tx_status(&ah->radio, &st);
}

/**
 * Read the status ring in order, as ath_tx_edma_tasklet does: each entry
 * the chip finished names its queue, whose oldest pushed frame it is. An
 * entry without the chip's vendor ID ends the pass, as in ath9k; a status
 * for a queue with nothing pushed is dropped. Each queue's waiting frames
 * then take the room the finished ones left in its FIFO.
 */
unsigned ath9k_tx_service(struct ath9k *ah)
{
    unsigned n = 0, i, ac;

    if (!ah->tx.status)
        return 0;
    for (i = 0; i < ATH9K_TXS_RING; i++) {
        struct ar9003_txs *ts = &ah->tx.status[ah->tx.status_index];
        unsigned qnum;

        k_barrier();
        if (!(ts->status8 & AR_TxDone))
            break;
        ah->tx.status_index = (ah->tx.status_index + 1) % ATH9K_TXS_RING;
        if (MS(ts->ds_info, AR_DescId) != ATH9K_VENDOR_ID || !(ts->ds_info & AR_TxRxDesc)) {
            k_memset(ts, 0, sizeof(*ts));
            break;
        }
        qnum = MS(ts->ds_info, AR_TxQcuNum);
        for (ac = 0; ac < ATH9K_TX_QUEUES; ac++)
            if (ah->tx.q[ac].qnum == qnum && ah->tx.q[ac].queued > 0) {
                complete(ah, &ah->tx.q[ac], ts);
                n++;
                break;
            }
        k_memset(ts, 0, sizeof(*ts));
    }
    for (ac = 0; ac < ATH9K_TX_QUEUES; ac++)
        kick(ah, &ah->tx.q[ac]);
    return n;
}

/**
 * Report every accepted frame failed, with no attempts, and empty the
 * queues, as ath_drain_all_txq does after the DMA is stopped; frames are
 * refused until ath9k_mac_init_queues sets the queues up again.
 */
void ath9k_tx_drain(struct ath9k *ah)
{
    unsigned ac, i;
    uint32_t flags = kirq_save();

    ah->tx.running = 0;
    for (ac = 0; ac < ATH9K_TX_QUEUES; ac++) {
        struct ath9k_txq *q = &ah->tx.q[ac];

        while (q->count > 0) {
            struct wlan_tx_status st;

            k_memset(&st, 0, sizeof(st));
            st.cookie = q->slot[q->head].info.cookie;
            for (i = 0; i < WLAN_TX_RATE_SERIES; i++)
                st.rates[i].index = -1;
            ah->stats.tx_failed++;
            q->head = (q->head + 1) % ATH9K_TX_RING;
            q->count--;
            wlan_radio_tx_status(&ah->radio, &st);
        }
        q->head = 0;
        q->queued = 0;
        q->inflight = 0;
    }
    kirq_restore(flags);
}
