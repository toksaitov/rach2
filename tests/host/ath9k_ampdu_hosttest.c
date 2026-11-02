/*
 * ath9k_ampdu_hosttest.c: transmit aggregation, from the station's ADDBA
 * request to the ath9k driver's A-MPDUs and the BlockAck's bitmap, against
 * the chip model of ath9k_model.h.
 *
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/mac.c
 * host-deps: rach2/sys/network/driver/ath9k/recv.c
 * host-deps: rach2/sys/network/driver/ath9k/xmit.c
 * host-deps: rach2/sys/network/driver/ath9k/ampdu.c
 * host-deps: tests/host/ath9k_model.c
 * host-deps: rach2/sys/network/wireless/tx.c rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The station's data path sends through a radio whose transmit and
 * BlockAck operations are the driver's, xmit.c and ampdu.c, on the model;
 * the test plays the AP, answering ADDBA requests and finishing each
 * descriptor chain with the status and BlockAck bitmap it chooses, and
 * reads every descriptor the driver pushed. Rate control picks MCS 7, 5
 * and 3 and then 1 Mbit/s. The rows check when the station asks for a
 * session and what it asks, the session's start, stop, refusal and
 * timeout, the aggregates' descriptors, lengths and delimiters, the
 * limits of the window, the TXOP and the AP, the holes a BlockAck leaves
 * sent again before new frames, a frame given up and the BlockAckReq past
 * it, a number skipped, an aggregate with no BlockAck, the drain of a
 * reset, and the boot option that turns aggregation off. It exits with
 * the count of failed checks, at most 255.
 */
#include "ath9k_model.h"
#include "network/wireless/data.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "mm/kheap.h"

extern int printf(const char *format, ...);

static unsigned fails;

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("ath9k_ampdu_hosttest: line %d: %s\n", (int)__LINE__, #c); \
            fails++;                                                          \
        }                                                                     \
    } while (0)

/** Whether the `n` bytes at `a` and `b` are equal. */
static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* DMA memory: pages whose bus addresses the model's dma hook maps.     */
/* ------------------------------------------------------------------ */

#define PAGES    200
#define BUS_BASE 0x10000000u

static unsigned char pages[PAGES][ATH9K_DMA_PAGE] __attribute__((aligned(4096)));
static unsigned pages_used;

void *ath9k_dma_alloc(struct ath9k *ah, uint32_t *bus)
{
    (void)ah;
    if (pages_used == PAGES)
        return 0;
    k_memset(pages[pages_used], 0, ATH9K_DMA_PAGE);
    *bus = BUS_BASE + pages_used * ATH9K_DMA_PAGE;
    return pages[pages_used++];
}

void ath9k_dma_free(struct ath9k *ah, void *page)
{
    (void)ah;
    (void)page;
}

static void *dma_map(struct ath9k_model *m, uint32_t bus, unsigned len)
{
    (void)m;
    if (bus < BUS_BASE || bus + len > BUS_BASE + pages_used * ATH9K_DMA_PAGE)
        return 0;
    return &pages[0][0] + (bus - BUS_BASE);
}

/* ------------------------------------------------------------------ */
/* The kernel and the parts not linked, as the code sees them.         */
/* ------------------------------------------------------------------ */

static uint64_t now = 1000;
static int noampdu; /* rach2.ath9k_noampdu is on the boot line */
static unsigned resets;
static enum ath9k_reset_reason reset_why;

uint64_t tick_count(void)
{
    return now;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The kirq_save regions open, with interrupts off while it is nonzero, and
 * its value at the last push of a BlockAckReq. */
static unsigned irq_off, bar_irq_off;

uint32_t kirq_save(void)
{
    return irq_off++;
}

void kirq_restore(uint32_t flags)
{
    irq_off = flags;
}

int kcmdline_has(const char *flag)
{
    return noampdu && same((const uint8_t *)flag, (const uint8_t *)"rach2.ath9k_noampdu", 20);
}

void ath9k_reset_schedule(struct ath9k *ah, enum ath9k_reset_reason why)
{
    (void)ah;
    resets++;
    reset_why = why;
}

int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    (void)work;
    (void)due;
    return 0;
}

void wlan_cancel_work(struct wlan_work *work)
{
    (void)work;
}

int wlan_radio_rx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                  const struct wlan_rx_status *status)
{
    (void)radio;
    (void)frame;
    (void)len;
    (void)status;
    return 0;
}

void ath9k_ant_rx(struct ath9k *ah, const struct ar9003_rxs *rxs)
{
    (void)ah;
    (void)rxs;
}

/* ------------------------------------------------------------------ */
/* The card and the station.                                           */
/* ------------------------------------------------------------------ */

static struct ath9k_model model;
static struct ath9k ah;
static struct wlan_data *d;
static struct wlan_counters counters;
static struct wlan_bss bss;

static const uint8_t station[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t ap[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
static const uint8_t far[6] = {0x02, 0x99, 0x99, 0x99, 0x99, 0x99};

/* The statuses the driver reported, by cookie and in order. */
#define MAX_TXS 512
static struct wlan_tx_status txs[MAX_TXS];
static unsigned txs_count, rated;

void wlan_radio_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    (void)radio;
    if (txs_count < MAX_TXS)
        txs[txs_count] = *status;
    txs_count++;
    if (status->rates[0].index >= 0)
        rated++;
    wlan_data_tx_status(d, status);
}

/** The times the cookie `cookie` was reported, and its last status. */
static unsigned reported(uint64_t cookie, struct wlan_tx_status *last)
{
    unsigned i, n = 0;

    for (i = 0; i < txs_count && i < MAX_TXS; i++)
        if (txs[i].cookie == cookie) {
            n++;
            if (last)
                *last = txs[i];
        }
    return n;
}

/* Rate control: MCS 7, 5 and 3, two tries each, then 1 Mbit/s twice. */
static void rc_select(struct wlan_radio *radio, const struct wlan_bss *peer, const uint8_t *frame,
                      unsigned len, struct wlan_tx_rate *rates)
{
    (void)radio;
    (void)peer;
    (void)frame;
    (void)len;
    rates[0].index = 7;
    rates[1].index = 5;
    rates[2].index = 3;
    rates[0].flags = rates[1].flags = rates[2].flags = WLAN_RATE_MCS;
    rates[3].index = 0;
    rates[3].flags = 0;
    rates[0].count = rates[1].count = rates[2].count = rates[3].count = 2;
}

static void rc_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    (void)radio;
    (void)status;
}

static const struct wlan_rate_ops rc_ops = {rc_select, rc_status};

/* The radio's operations: the driver's transmit and BlockAck, keys in
 * software, and nothing to start or tune. */
static int radio_start(struct wlan_radio *radio)
{
    (void)radio;
    return 0;
}

static void radio_stop(struct wlan_radio *radio)
{
    (void)radio;
}

static int radio_config(struct wlan_radio *radio, const struct wlan_channel *channel,
                        unsigned filters)
{
    (void)radio;
    (void)channel;
    (void)filters;
    return 0;
}

static int radio_tx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                    const struct wlan_tx_info *info)
{
    return ath9k_tx(radio->priv, frame, len, info);
}

static int radio_set_key(struct wlan_radio *radio, int enable, const uint8_t peer[6],
                         const struct wlan_key *key, int *slot)
{
    (void)radio;
    (void)enable;
    (void)peer;
    (void)key;
    (void)slot;
    return -EOPNOTSUPP;
}

static int radio_ampdu(struct wlan_radio *radio, enum wlan_ampdu_action action,
                       const uint8_t peer[6], unsigned tid, unsigned ssn, unsigned window)
{
    return ath9k_ampdu_action(radio->priv, action, peer, tid, ssn, window);
}

static const struct wlan_radio_ops ops = {
    radio_start, radio_stop, radio_config, radio_tx, radio_set_key, 0, radio_ampdu, 0, 0, 0};

/** The AP's elements: its SSID, rates and HT Capabilities, whose A-MPDU
 * parameters are `ampdu`, or none when `ht` is 0. */
static void ap_ies(int ht, unsigned ampdu)
{
    static const uint8_t base[] = {0, 3, 'a', 'p', 'x', 1, 4, 0x82, 0x84, 0x0c, 0x18};
    unsigned n = sizeof(base);

    k_memcpy(bss.ies, base, n);
    if (ht) {
        k_memset(bss.ies + n, 0, 2 + IEEE80211_HT_CAP_LEN);
        bss.ies[n] = WLAN_EID_HT_CAPABILITY;
        bss.ies[n + 1] = IEEE80211_HT_CAP_LEN;
        bss.ies[n + 4] = (uint8_t)ampdu;
        n += 2 + IEEE80211_HT_CAP_LEN;
    }
    bss.ies_len = n;
}

static void model_write(struct ath9k_model *m, uint32_t reg, uint32_t value);

/**
 * A started card with its queues set up, as a reset leaves them, and a
 * station connected with QoS to an open HT AP whose A-MPDUs are up to
 * 64 KiB with a 4-microsecond spacing (factor 3, spacing code 5).
 */
static void setup(void)
{
    static const uint16_t rates[12] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};
    const struct ath9k_chip *chip = ath9k_chip_find(0x0036);
    unsigned i;

    k_memset(&ah, 0, sizeof(ah));
    pages_used = 0;
    txs_count = rated = 0;
    resets = 0;
    noampdu = 0;
    ath9k_model_init(&model, chip, chip->revs[0].mac_rev);
    model.hooks.dma = dma_map;
    model.hooks.write = model_write;
    ath9k_model_attach(&model, &ah);
    ath9k_model_poke(&model, AR_RTC_RESET, AR_RTC_RESET_EN);
    ath9k_model_poke(&model, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    ah.hw.chip = chip;
    for (i = 0; i < 12; i++)
        ah.radio.band.rates[i] = rates[i];
    ah.radio.band.n_rates = 12;
    ah.radio.band.ht_supported = 1;
    ah.radio.ops = &ops;
    ah.radio.priv = &ah;
    ah.radio.rate_ops = &rc_ops;
    k_memcpy(ah.radio.mac, station, 6);
    CHECK(ath9k_tx_init(&ah) == 0);
    ath9k_mac_init_queues(&ah);

    k_memset(&bss, 0, sizeof(bss));
    k_memcpy(bss.bssid, ap, 6);
    bss.frequency = 2437;
    bss.capability = WLAN_CAPABILITY_ESS;
    ap_ies(1, 3 | 5 << 2);
    ath9k_ampdu_sta(&ah, &bss);
    k_memset(&counters, 0, sizeof(counters));
    if (!d)
        d = kzalloc(sizeof(*d));
    wlan_data_init(d, 0, &ah.radio, &counters, 0, 0);
    CHECK(wlan_data_connect(d, &bss, 1, 0, 0) == 0);
}

/** Send an IPv4 datagram of `body` bytes, or an EAPOL frame, through the
 * station at 802.1D priority `prio`. Returns wlan_data_tx's answer. */
static int send(unsigned prio, unsigned body, int eapol)
{
    static const uint8_t snap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00};
    uint8_t f[IEEE80211_MAX_FRAME_LEN];
    unsigned i;

    k_memset(f, 0, sizeof(f));
    f[0] = 0x08; /* data */
    f[1] = 0x01; /* to the DS */
    k_memcpy(f + 4, ap, 6);
    k_memcpy(f + 10, station, 6);
    k_memcpy(f + 16, far, 6);
    k_memcpy(f + 24, snap, 8);
    if (eapol) {
        f[30] = 0x88;
        f[31] = 0x8e;
    }
    for (i = 0; i < body; i++)
        f[32 + i] = (uint8_t)i;
    return wlan_data_tx(d, f, 32 + body, sizeof(f), prio);
}

/** The descriptor at bus address `bus`, or NULL. */
static struct ar9003_txc *desc_at(uint32_t bus)
{
    return dma_map(&model, bus, sizeof(struct ar9003_txc));
}

/** The descriptors of the `k`th oldest chain in queue `q`'s FIFO, by
 * their links, into `out`; returns how many. */
static unsigned chain_of(unsigned q, unsigned k, struct ar9003_txc **out, unsigned max)
{
    const struct ath9k_model_fifo *f = &model.tx[q];
    uint32_t bus;
    unsigned n = 0;

    out[0] = 0;
    if (k >= f->count)
        return 0;
    bus = f->bus[(f->head + k) % f->depth];
    while (bus && n < max) {
        out[n] = desc_at(bus);
        if (!out[n])
            break;
        bus = out[n++]->link;
    }
    return n;
}

/** The frame a descriptor sends: its buffer, the chip's padding in it. */
static const uint8_t *frame_of(const struct ar9003_txc *dc)
{
    return dc ? dma_map(&model, dc->data0, MS(dc->ctl3, AR_BufLen)) : 0;
}

/** Note the interrupt regions open at each push of a BlockAckReq. */
static void model_write(struct ath9k_model *m, uint32_t reg, uint32_t value)
{
    const uint8_t *f;

    (void)m;
    if (reg < AR_QTXDP(0) || reg > AR_QTXDP(AR_NUM_QCU - 1))
        return;
    f = frame_of(desc_at(value));
    if (f && f[0] == 0x84)
        bar_irq_off = irq_off;
}

/** The sequence number of the QoS data frame a descriptor sends. */
static unsigned seq_of(const struct ar9003_txc *dc)
{
    const uint8_t *f = frame_of(dc);

    return f ? ((unsigned)f[22] | (unsigned)f[23] << 8) >> 4 : 0xffff;
}

/** Finish queue `q`'s oldest chain as the chip does, with status words
 * `s3` and `s8`, and a BlockAck of `bits` from `seq` when `ba` is set;
 * then serve the status ring. Returns the chain's bus address. */
static uint32_t finish(unsigned q, uint32_t s3, uint32_t s8, int ba, unsigned seq, uint64_t bits)
{
    struct ar9003_txs st;
    uint32_t bus;

    k_memset(&st, 0, sizeof(st));
    st.status3 = s3;
    st.status8 = s8 | SM(seq, AR_SeqNum);
    if (ba) {
        st.status2 = AR_TxBaStatus;
        st.status5 = (uint32_t)bits;
        st.status6 = (uint32_t)(bits >> 32);
    }
    bus = ath9k_model_tx_complete(&model, q, &st);
    ath9k_tx_service(&ah);
    return bus;
}

/** Finish queue `q`'s oldest chain acknowledged at its first series: a
 * frame alone by its ACK, an aggregate by a BlockAck of every subframe
 * from the lowest number among them. */
static uint32_t finish_ok(unsigned q)
{
    struct ar9003_txc *dc[ATH9K_BA_WINDOW];
    unsigned n = chain_of(q, 0, dc, ATH9K_BA_WINDOW), i, base;
    uint64_t bits = 0;

    if (n < 2 || !(dc[0]->ctl12 & AR_IsAggr))
        return finish(q, AR_FrmXmitOK, 0, 0, 0, 0);
    base = seq_of(dc[0]);
    for (i = 1; i < n; i++)
        if (((base - seq_of(dc[i])) & 0xfff) < ATH9K_BA_WINDOW)
            base = seq_of(dc[i]);
    for (i = 0; i < n; i++)
        bits |= (uint64_t)1 << ((seq_of(dc[i]) - base) & 0x3f);
    return finish(q, AR_FrmXmitOK, 0, 1, base, bits);
}

/** The AP's answer to an ADDBA request: dialog `token`, `status`, the
 * BlockAck parameters `params`. Returns wlan_data_rx_action's answer. */
static int addba_resp(unsigned token, unsigned status, unsigned params)
{
    uint8_t f[IEEE80211_HDRLEN + 9];
    uint8_t *p = f + IEEE80211_HDRLEN;

    k_memset(f, 0, sizeof(f));
    f[0] = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION;
    k_memcpy(f + 4, station, 6);
    k_memcpy(f + 10, ap, 6);
    k_memcpy(f + 16, ap, 6);
    p[0] = WLAN_CATEGORY_BACK;
    p[1] = WLAN_ACTION_ADDBA_RESP;
    p[2] = (uint8_t)token;
    p[3] = (uint8_t)status;
    p[4] = (uint8_t)(status >> 8);
    p[5] = (uint8_t)params;
    p[6] = (uint8_t)(params >> 8);
    return wlan_data_rx_action(d, f, sizeof(f), 0);
}

/** The AP's DELBA of TID `tid`, as the originator when `initiator`. */
static int delba(unsigned tid, int initiator)
{
    uint8_t f[IEEE80211_HDRLEN + 6];
    unsigned params = (initiator ? 0x0800 : 0) | tid << 12;

    k_memset(f, 0, sizeof(f));
    f[0] = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION;
    k_memcpy(f + 4, station, 6);
    k_memcpy(f + 10, ap, 6);
    k_memcpy(f + 16, ap, 6);
    f[24] = WLAN_CATEGORY_BACK;
    f[25] = WLAN_ACTION_DELBA;
    f[26] = (uint8_t)params;
    f[27] = (uint8_t)(params >> 8);
    f[28] = 1;
    return wlan_data_rx_action(d, f, sizeof(f), 0);
}

/** The Block Ack action frame at the head of the VO queue, which the
 * station sends at once: its body, or NULL. */
static const uint8_t *action_sent(void)
{
    struct ar9003_txc *dc[1];
    const uint8_t *f;

    if (chain_of(3, model.tx[3].count - 1, dc, 1) != 1)
        return 0;
    f = frame_of(dc[0]);
    if (!f || f[0] != (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION) ||
        f[24] != WLAN_CATEGORY_BACK)
        return 0;
    return f + 24;
}

/** Empty every queue's FIFO, each chain acknowledged. */
static void finish_all(void)
{
    unsigned q;

    for (q = 0; q < 4; q++)
        while (model.tx[q].count)
            (void)finish_ok(q);
}

/** Start a session of TID 0 with a window of `window` frames, as the
 * station does after its first frame: the request answered with success. */
static void establish(unsigned window)
{
    const uint8_t *req;

    CHECK(send(0, 100, 0) == 0);
    req = action_sent();
    CHECK(req && req[1] == WLAN_ACTION_ADDBA_REQ);
    if (req)
        CHECK(addba_resp(req[2], 0, 0x0002 | window << 6) == 0);
    CHECK(ah.ampdu.tid[0].active && ah.ampdu.tid[0].window == window);
    finish_all();
    txs_count = rated = 0;
}

/* ------------------------------------------------------------------ */
/* The rows.                                                            */
/* ------------------------------------------------------------------ */

/* When the station asks for a session, what it asks, and its start. */
static void test_request(void)
{
    const uint8_t *req;
    unsigned params, ssn;

    setup();
    CHECK(ah.ampdu.max_len == 65535 && ah.ampdu.density == 4);

    /* Voice and EAPOL get no session. */
    CHECK(send(6, 100, 0) == 0);
    CHECK(model.tx[3].count == 1 && d->ba_tx[6].state == WLAN_BA_TX_IDLE);
    CHECK(send(0, 100, 1) == 0);
    CHECK(model.tx[1].count == 1 && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
    finish_all();

    /* The first other frame of TID 0 asks, for immediate BlockAck of 64
     * frames with no timeout, from the TID's next number, 2. */
    CHECK(send(0, 100, 0) == 0);
    CHECK(model.tx[1].count == 1 && model.tx[3].count == 1);
    req = action_sent();
    CHECK(req != 0);
    if (req) {
        params = (unsigned)req[3] | (unsigned)req[4] << 8;
        ssn = ((unsigned)req[7] | (unsigned)req[8] << 8) >> 4;
        CHECK(req[1] == WLAN_ACTION_ADDBA_REQ && params == (0x0002 | 0 << 2 | 64 << 6));
        CHECK(req[5] == 0 && req[6] == 0 && ssn == 2 && (req[7] & 0x0f) == 0);
        CHECK(d->ba_tx[0].state == WLAN_BA_TX_REQUESTED && d->ba_tx[0].token == req[2]);

        /* One request at a time; a wrong token or TID is ignored. */
        CHECK(send(0, 100, 0) == 0);
        CHECK(model.tx[3].count == 1);
        CHECK(addba_resp(req[2] + 1u, 0, 0x0002 | 64 << 6) == 0);
        CHECK(addba_resp(req[2], 0, 0x0002 | 3 << 2 | 64 << 6) == 0);
        CHECK(!ah.ampdu.tid[0].active && d->ba_tx[0].state == WLAN_BA_TX_REQUESTED);

        /* The answer starts the session in the radio at the request's
         * number, with the AP's window. */
        CHECK(addba_resp(req[2], 0, 0x0002 | 32 << 6) == 0);
        CHECK(d->ba_tx[0].state == WLAN_BA_TX_ACTIVE && d->ba_tx[0].window == 32);
        CHECK(ah.ampdu.tid[0].active && ah.ampdu.tid[0].window == 32);
        CHECK(ah.ampdu.tid[0].seq_start == 2 && ah.ampdu.tid[0].seq_next == 2);
        CHECK(same(ah.ampdu.peer, ap, 6));
    }
    finish_all();

    /* An AP without HT Capabilities is not asked. */
    setup();
    ap_ies(0, 0);
    CHECK(send(0, 100, 0) == 0);
    CHECK(model.tx[3].count == 0 && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
}

/* A refusal and a request unanswered wait 15 seconds; a DELBA ends it. */
static void test_refusal(void)
{
    const uint8_t *req;

    setup();
    CHECK(send(0, 100, 0) == 0);
    req = action_sent();
    CHECK(req != 0);
    if (req)
        CHECK(addba_resp(req[2], WLAN_STATUS_REQUEST_DECLINED, 0x0002 | 64 << 6) == 0);
    CHECK(d->ba_tx[0].state == WLAN_BA_TX_IDLE && !ah.ampdu.tid[0].active);
    finish_all();
    now += WLAN_BA_TX_RETRY - 1;
    CHECK(send(0, 100, 0) == 0);
    CHECK(model.tx[3].count == 0);
    finish_all();
    now += 1;
    CHECK(send(0, 100, 0) == 0);
    CHECK(model.tx[3].count == 1 && d->ba_tx[0].state == WLAN_BA_TX_REQUESTED);
    finish_all();

    /* No answer in a second: the next frame gives up and waits again. */
    now += WLAN_BA_TX_RESPONSE;
    CHECK(send(0, 100, 0) == 0);
    CHECK(model.tx[3].count == 0 && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
    CHECK(d->ba_tx[0].retry_at == now + WLAN_BA_TX_RETRY);
    finish_all();

    /* The AP's DELBA as recipient stops the radio's session; one as
     * originator leaves it. */
    setup();
    establish(64);
    CHECK(delba(0, 1) == 0);
    CHECK(ah.ampdu.tid[0].active && d->ba_tx[0].state == WLAN_BA_TX_ACTIVE);
    CHECK(delba(0, 0) == 0);
    CHECK(!ah.ampdu.tid[0].active && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
    CHECK(d->ba_tx[0].retry_at == now + WLAN_BA_TX_RETRY);

    /* A disconnection stops it too. */
    setup();
    establish(64);
    wlan_data_disconnect(d);
    CHECK(!ah.ampdu.tid[0].active && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
}

/* rach2.ath9k_noampdu: the radio refuses, the station sends a DELBA and
 * asks no more on this connection. */
static void test_option(void)
{
    const uint8_t *req;

    setup();
    noampdu = 1;
    CHECK(send(0, 100, 0) == 0);
    req = action_sent();
    CHECK(req != 0);
    if (req)
        CHECK(addba_resp(req[2], 0, 0x0002 | 64 << 6) == 0);
    CHECK(!ah.ampdu.tid[0].active && (ah.options & ATH9K_OPT_NO_AMPDU) && d->ba_tx_off);
    req = action_sent();
    CHECK(req && req[1] == WLAN_ACTION_DELBA && req[2] == 0x00 && req[3] == 0x08 && req[4] == 37);
    finish_all();
    now += WLAN_BA_TX_RETRY;
    CHECK(send(0, 100, 0) == 0);
    CHECK(model.tx[3].count == 0);
    CHECK(ath9k_ampdu_action(&ah, WLAN_AMPDU_TX_START, ap, 0, 0, 64) == -EOPNOTSUPP);
    CHECK(ath9k_ampdu_action(&ah, WLAN_AMPDU_RX_START, ap, 0, 0, 64) == -EOPNOTSUPP);
}

/* The descriptors of an aggregate of eight 1000-byte frames whose first
 * has sequence number SEQ, sent after a lone frame that took SINGLE_DUR. */
static void check_eight(struct ar9003_txc **dc, unsigned seq, unsigned single_dur)
{
    unsigned i;
    uint32_t sum;

    /* 1038 bytes each with the FCS, a delimiter before each and two bytes
     * of padding after all but the last. */
    CHECK(MS(dc[0]->ctl17, AR_AggrLen) == 8 * (4 + 1038) + 7 * 2);
    for (i = 0; i < 8; i++) {
        CHECK((dc[i]->ctl12 & AR_IsAggr) && !!(dc[i]->ctl12 & AR_MoreAggr) == (i + 1 < 8));
        CHECK((dc[i]->ctl11 & AR_FrameLen) == 1038 && MS(dc[i]->ctl17, AR_PadDelim) == 0);
        CHECK(seq_of(dc[i]) == ((seq + i) & 0xfff));
        CHECK(MS(dc[i]->ctl14, AR_XmitRate0) == 0x87 && MS(dc[i]->ctl14, AR_XmitRate2) == 0x83);
        CHECK(MS(dc[i]->ctl13, AR_XmitDataTries3) == 0 && MS(dc[i]->ctl14, AR_XmitRate3) == 0);
        CHECK(MS(dc[i]->ctl15, AR_PacketDur0) > 8 * single_dur / 2);
        sum = dc[i]->info + dc[i]->link + dc[i]->data0 + dc[i]->ctl3;
        CHECK((dc[i]->ctl10 & AR_TxPtrChkSum) == (((sum & 0xffff) + (sum >> 16)) & 0xffff));
    }
    CHECK(dc[7]->link == 0 && MS(dc[7]->ctl17, AR_AggrLen) == 0);
}

/* The rest of test_aggregate's ten frames, the first of which, sequence
 * number S0 and cookie COOKIE0, is acknowledged and the next eight are on
 * the air as one aggregate behind the second. */
static void ack_with_hole(unsigned s0, uint64_t cookie0)
{
    struct ar9003_txc *dc[40];
    struct wlan_tx_status st;
    const uint8_t *f;
    unsigned i;

    /* The second alone, then the aggregate's BlockAck with the fourth
     * missing: seven are done, one stays with its Retry bit set and goes
     * again, alone. */
    (void)finish_ok(1);
    CHECK(txs_count == 2);
    (void)finish(1, AR_FrmXmitOK, 0, 1, (s0 + 2) & 0xfff, 0xff & ~(uint64_t)0x08);
    CHECK(txs_count == 9 && rated == 3 && ah.ampdu.sw_retries == 1);
    CHECK(reported(cookie0 + 5, 0) == 0 && reported(cookie0 + 6, &st) == 1);
    CHECK(st.flags == WLAN_TX_ACKED && st.rates[0].index == -1);
    CHECK(reported(cookie0 + 2, &st) == 1 && st.rates[0].index == 7 && st.rates[3].index == -1);
    CHECK(model.tx[1].count == 1 && chain_of(1, 0, dc, 40) == 1);
    CHECK(seq_of(dc[0]) == ((s0 + 5) & 0xfff) && !(dc[0]->ctl12 & AR_IsAggr));
    f = frame_of(dc[0]);
    CHECK(f && (f[1] & (IEEE80211_FCTL_RETRY >> 8)));
    CHECK(ah.ampdu.tid[0].seq_start == ((s0 + 5) & 0xfff));
    (void)finish_ok(1);
    CHECK(txs_count == 10 && reported(cookie0 + 5, &st) == 1 && st.flags == WLAN_TX_ACKED);
    for (i = 0; i < 10; i++)
        CHECK(reported(cookie0 + i, 0) == 1);
    CHECK(ah.ampdu.tid[0].out == 0 && ah.ampdu.tid[0].seq_start == ((s0 + 10) & 0xfff));
    CHECK(counters.tx_failed == 0 && ah.tx.q[WLAN_AC_BE].count == 0 && model.tx[1].count == 0);
}

/* Aggregates: their descriptors, the BlockAck read back, a hole sent
 * again before the new frames, and one status for rate control each. */
static void test_aggregate(void)
{
    struct ar9003_txc *dc[40];
    struct wlan_tx_status st;
    uint64_t cookie0;
    unsigned n, i, s0, single_dur;

    setup();
    establish(64);
    cookie0 = WLAN_DATA_COOKIE | WLAN_DATA_COOKIE_DATA | (d->next_cookie + 1);

    /* Ten frames: two go alone, in their window, and eight wait. */
    for (i = 0; i < 10; i++)
        CHECK(send(0, 1000, 0) == 0);
    CHECK(model.tx[1].count == 2 && ah.tx.q[WLAN_AC_BE].count == 10);
    CHECK(chain_of(1, 0, dc, 40) == 1);
    s0 = seq_of(dc[0]);
    CHECK(!(dc[0]->ctl12 & AR_IsAggr) && dc[0]->link == 0);
    CHECK(MS(dc[0]->ctl13, AR_XmitDataTries3) == 2 && MS(dc[0]->ctl14, AR_XmitRate3) == 0x1b);
    single_dur = MS(dc[0]->ctl15, AR_PacketDur0);
    CHECK(ah.ampdu.tid[0].seq_start == s0 && ah.ampdu.tid[0].out == 3);

    /* The first acknowledged: the eight go as one aggregate. */
    (void)finish_ok(1);
    CHECK(reported(cookie0, &st) == 1 && st.flags == WLAN_TX_ACKED);
    CHECK(model.tx[1].count == 2 && ah.ampdu.aggregates == 1 && ah.ampdu.subframes == 8);
    n = chain_of(1, 1, dc, 40);
    CHECK(n == 8);
    if (n == 8)
        check_eight(dc, s0 + 2, single_dur);

    ack_with_hole(s0, cookie0);

    /* Short frames: delimiters up to the chip's 256 bytes. */
    for (i = 0; i < 4; i++)
        CHECK(send(0, 50, 0) == 0);
    (void)finish_ok(1);
    n = chain_of(1, 1, dc, 40);
    CHECK(n == 2);
    if (n == 2)
        CHECK(MS(dc[0]->ctl17, AR_PadDelim) == (256 - 88 + 3) / 4 &&
              MS(dc[0]->ctl17, AR_AggrLen) == 2 * (4 + 88) + (256 - 88 + 3) / 4 * 4);
    finish_all();
    CHECK(model.overruns == 0 && model.dma_faults == 0);
}

/* The limits: half the window per aggregate, frames past the window wait,
 * the AP's length, and the TXOP of the slowest series. */
static void test_limits(void)
{
    struct ar9003_txc *dc[40];
    unsigned i, s0;

    setup();
    establish(4);
    for (i = 0; i < 8; i++)
        CHECK(send(0, 1000, 0) == 0);
    CHECK(chain_of(1, 0, dc, 40) == 1);
    s0 = seq_of(dc[0]);
    (void)finish_ok(1);
    CHECK(chain_of(1, 1, dc, 40) == 2);
    (void)finish_ok(1);
    CHECK(chain_of(1, 1, dc, 40) == 2 && seq_of(dc[0]) == ((s0 + 4) & 0xfff));
    CHECK(ah.tx.q[WLAN_AC_BE].count == 6 && ah.tx.q[WLAN_AC_BE].queued == 4);

    /* A hole at the window's start holds the next frames back: the hole
     * goes alone, and the frame four past it waits. */
    (void)finish(1, AR_FrmXmitOK, 0, 1, (s0 + 2) & 0xfff, 0x2);
    CHECK(chain_of(1, 1, dc, 40) == 1 && seq_of(dc[0]) == ((s0 + 2) & 0xfff));
    CHECK(ah.tx.q[WLAN_AC_BE].queued == 3 && model.tx[1].count == 2);
    (void)finish_ok(1);
    CHECK(model.tx[1].count == 1);
    (void)finish_ok(1);
    CHECK(chain_of(1, 0, dc, 40) == 2 && seq_of(dc[0]) == ((s0 + 6) & 0xfff));
    finish_all();
    CHECK(ah.tx.q[WLAN_AC_BE].count == 0 && txs_count == 8);

    /* An AP of the least A-MPDU length, 8191 bytes, takes seven. */
    setup();
    ap_ies(1, 0);
    ath9k_ampdu_sta(&ah, &bss);
    CHECK(ah.ampdu.max_len == 8191 && ah.ampdu.density == 0);
    establish(64);
    for (i = 0; i < 12; i++)
        CHECK(send(0, 1000, 0) == 0);
    (void)finish_ok(1);
    CHECK(chain_of(1, 1, dc, 40) == 7);
    finish_all();

    /* No HT Capabilities known: the most cautious. */
    ath9k_ampdu_sta(&ah, 0);
    CHECK(ah.ampdu.max_len == 8191 && ah.ampdu.density == 16);

    /* Video's 3 ms TXOP at MCS 3 holds about 9800 bytes: nine frames. */
    setup();
    CHECK(send(4, 100, 0) == 0);
    {
        const uint8_t *req = action_sent();

        CHECK(req && ((unsigned)req[3] >> 2 & 0xf) == 4);
        if (req)
            CHECK(addba_resp(req[2], 0, 0x0002 | 4 << 2 | 64 << 6) == 0);
    }
    finish_all();
    for (i = 0; i < 14; i++)
        CHECK(send(4, 1000, 0) == 0);
    (void)finish_ok(2);
    CHECK(chain_of(2, 1, dc, 40) == 9);
    finish_all();
}

/* A frame given up after its tries, and the BlockAckReq past it. */
static void test_give_up(void)
{
    struct ar9003_txc *dc[4];
    struct wlan_tx_status st;
    const uint8_t *f;
    uint64_t cookie;
    unsigned s0, ssn, txs_before, i;

    setup();
    establish(64);
    cookie = WLAN_DATA_COOKIE | WLAN_DATA_COOKIE_DATA | (d->next_cookie + 1);
    CHECK(send(0, 500, 0) == 0);
    CHECK(chain_of(1, 0, dc, 4) == 1);
    s0 = seq_of(dc[0]);

    /* Every try of the four series fails, eight attempts a push: kept
     * three times, 24 attempts, then sent with the six left of its 30, the
     * last series ended, and given up with the tries of that push. */
    for (i = 0; i < 3; i++) {
        (void)finish(1, AR_ExcessiveRetries | SM(1, AR_DataFailCnt), SM(3, AR_FinalTxIdx), 0, 0, 0);
        CHECK(reported(cookie, 0) == 0 && model.tx[1].count == 1);
    }
    CHECK(chain_of(1, 0, dc, 4) == 1);
    CHECK(MS(dc[0]->ctl13, AR_XmitDataTries0) == 2 && MS(dc[0]->ctl13, AR_XmitDataTries1) == 2);
    CHECK(MS(dc[0]->ctl13, AR_XmitDataTries2) == 2 && MS(dc[0]->ctl13, AR_XmitDataTries3) == 0);
    txs_before = txs_count;
    (void)finish(1, AR_ExcessiveRetries | SM(1, AR_DataFailCnt), SM(2, AR_FinalTxIdx), 0, 0, 0);
    CHECK(reported(cookie, &st) == 1 && st.flags == 0 && st.rates[2].count == 2);
    CHECK(st.rates[3].index == -1);
    CHECK(counters.tx_failed == 1 && ah.ampdu.sw_retries == 3);

    /* The window moved past it, and a BlockAckReq for its successor went
     * out on its queue, compressed, of TID 0, at 1 Mbit/s. */
    CHECK(ah.ampdu.bars == 1 && !ah.ampdu.tid[0].bar_owed && model.tx[1].count == 1);
    CHECK(chain_of(1, 0, dc, 4) == 1);
    f = frame_of(dc[0]);
    CHECK(f != 0);
    if (f) {
        ssn = ((unsigned)f[18] | (unsigned)f[19] << 8) >> 4;
        CHECK(f[0] == 0x84 && f[1] == 0 && same(f + 4, ap, 6) && same(f + 10, station, 6));
        CHECK(f[16] == 0x04 && f[17] == 0x00 && ssn == ((s0 + 1) & 0xfff));
        CHECK((dc[0]->ctl11 & AR_FrameLen) == 24 && MS(dc[0]->ctl14, AR_XmitRate0) == 0x1b);
    }
    /* Its status is the driver's own. */
    (void)finish_ok(1);
    CHECK(txs_count == txs_before + 1 && ah.tx.q[WLAN_AC_BE].count == 0);
}

/* An aggregate that went out but had no BlockAck asks for a reset, and its
 * frames go again. */
static void test_no_blockack(void)
{
    unsigned i;

    setup();
    establish(64);
    for (i = 0; i < 6; i++)
        CHECK(send(0, 1000, 0) == 0);
    (void)finish_ok(1);
    (void)finish_ok(1);
    CHECK(ah.tx.q[WLAN_AC_BE].count == 4 && ah.tx.q[WLAN_AC_BE].chains == 1);
    (void)finish(1, AR_FrmXmitOK, 0, 0, 0, 0);
    CHECK(resets == 1 && reset_why == ATH9K_RESET_TX_ERROR);
    CHECK(txs_count == 2 && ah.tx.q[WLAN_AC_BE].count == 4 && ah.ampdu.sw_retries == 4);
    finish_all();
    CHECK(txs_count == 6);
}

/* A number skipped: a frame the radio refused leaves none, a reset's drain
 * owes a BlockAckReq, and a session's stop sends the rest alone. */
static void test_gaps(void)
{
    struct ar9003_txc *dc[40];
    const uint8_t *f;
    unsigned i, seq, s;

    setup();
    establish(64);

    /* A full queue refuses, and the number goes to the next frame. */
    for (i = 0; i < ATH9K_TX_RING; i++)
        CHECK(send(0, 100, 0) == 0);
    seq = d->tx_seq[0];
    CHECK(send(0, 100, 0) == -ENOBUFS && d->tx_seq[0] == seq);
    finish_all();
    CHECK(ah.tx.q[WLAN_AC_BE].count == 0 && txs_count == ATH9K_TX_RING && ah.ampdu.bars == 0);
    CHECK(ah.ampdu.tid[0].seq_start == seq && ah.ampdu.tid[0].out == 0);

    /* A reset drops what was queued: each frame failed once, and the next
     * frame's BlockAckReq starts at it. */
    txs_count = 0;
    for (i = 0; i < 4; i++)
        CHECK(send(0, 100, 0) == 0);
    (void)ath9k_mac_abort_tx(&ah);
    CHECK(model.tx[1].count == 0);
    ath9k_tx_drain(&ah);
    CHECK(txs_count == 4 && counters.tx_failed == 4 && ah.ampdu.tid[0].bar_owed);
    ath9k_mac_init_queues(&ah);
    s = d->tx_seq[0];
    CHECK(send(0, 100, 0) == 0);
    CHECK(ah.ampdu.bars == 1 && model.tx[1].count == 2 && chain_of(1, 1, dc, 40) == 1);
    f = frame_of(dc[0]);
    CHECK(f && f[0] == 0x84 && (((unsigned)f[18] | (unsigned)f[19] << 8) >> 4) == s);
    /* The worker chose and queued it with interrupts off before its queue
     * took them off again, so that no completion came between. */
    CHECK(bar_irq_off >= 2 && irq_off == 0);
    finish_all();

    /* After a stop the frames go alone, outside any window. */
    CHECK(delba(0, 0) == 0 && !ah.ampdu.tid[0].active);
    for (i = 0; i < 5; i++)
        CHECK(send(0, 1000, 0) == 0);
    CHECK(model.tx[1].count == 5);
    for (i = 0; i < 5; i++) {
        CHECK(chain_of(1, i, dc, 40) == 1 && !(dc[0]->ctl12 & AR_IsAggr));
    }
    CHECK(!ah.tx.q[WLAN_AC_BE].slot[ah.tx.q[WLAN_AC_BE].head].ba);
    finish_all();
    CHECK(ah.tx.q[WLAN_AC_BE].count == 0);
}

int main(void)
{
    test_request();
    test_refusal();
    test_option();
    test_aggregate();
    test_limits();
    test_give_up();
    test_no_blockack();
    test_gaps();
    kfree(d);
    if (fails)
        printf("ath9k_ampdu_hosttest: %u failed\n", fails);
    return fails > 255 ? 255 : (int)fails;
}
