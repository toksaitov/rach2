/* Check the wireless transmit queue's back-pressure: a radio whose ring is
 * full holds the queue's frames until its completions free slots, in order
 * and with none dropped, the queue keeps its last slots for EAPOL, another
 * refusal and a stop still drop frames, a pairwise key waits for what was
 * queued before it, the receive queue holds a pass of a radio's buffers, a
 * stream of received frames holds up no timer, and no frame's completion is
 * lost to a report of tries alone.
 * host-deps: rach2/sys/network/wireless/data.c
 * host-deps: rach2/sys/network/wireless/tx.c
 * host-deps: rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/wlan.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 */
#include "network/wireless/data.h"
#include "network/wireless/wiphy.h"
#include "network/wireless/work.h"
#include "network/ethernet/ethernet.h"
#include "network/net.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "lib/kirq.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                                \
    do {                                                                        \
        if (!(x)) {                                                             \
            printf("wlan_txq_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

/* The radio's ring, smaller than the adapter's queue so that the queue
 * fills behind it. */
#define RING 4

/* Where an Ethernet frame's mark, its number, lands: byte 10 of the
 * payload, which follows a 24-byte header and an 8-byte LLC/SNAP header. */
#define ETH_MARK  (ETH_HDR_SIZE + 10)
#define MPDU_MARK (IEEE80211_HDRLEN + 8 + 10)

static const uint8_t sta[6] = {0x02, 0, 0, 0, 0, 0x01};
static const uint8_t bssid[6] = {0x02, 0, 0, 0, 0, 0xaa};
static const uint8_t far[6] = {0x02, 0, 0, 0, 0, 0xbb};

/* --- The kernel's calls the data path makes, modelled on one thread. --- */

static uint64_t now;
static struct wlan_work *queued[8];
static unsigned long order[8], next_order; /* the order records were queued */
static struct wlan_work *running;          /* the record whose callback runs */

/** The network lock: nothing to exclude on one thread. */
uint32_t net_lock(void)
{
    return 0;
}

/** Release the network lock: nothing to restore. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** No interrupts to mask. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing was masked. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** The kernel's tick, which only a row that models time moves. */
uint64_t tick_count(void)
{
    return now;
}

/** Queue work as work.c does, refusing a record already queued or running;
 * immediate work is due at the tick it was queued. */
int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    unsigned i;

    if (work->pending || work == running)
        return -EBUSY;
    for (i = 0; i < 8; ++i) {
        if (!queued[i]) {
            queued[i] = work;
            order[i] = next_order++;
            work->pending = 1;
            work->due = due ? due : now;
            return 0;
        }
    }
    return -EBUSY;
}

/** Take queued work off the queue unrun. */
void wlan_cancel_work(struct wlan_work *work)
{
    unsigned i;

    for (i = 0; i < 8; ++i)
        if (queued[i] == work)
            queued[i] = 0;
    work->pending = 0;
}

/** The number of records queued. */
static unsigned pending(void)
{
    unsigned i, n = 0;

    for (i = 0; i < 8; ++i)
        n += queued[i] != 0;
    return n;
}

/** Run queued work, and the work it queues, until none is left, oldest
 * record first as the worker runs records of one due tick. */
static void run_work(void)
{
    struct wlan_work *w;
    unsigned i;

    for (;;) {
        for (i = 0; i < 8 && !queued[i]; ++i)
            ;
        if (i == 8)
            return;
        w = queued[i];
        for (; i < 7; ++i) {
            queued[i] = queued[i + 1];
            order[i] = order[i + 1];
        }
        queued[7] = 0;
        w->pending = 0;
        running = w;
        w->run(w);
        running = 0;
    }
}

/** Run the earliest due record, by due tick and then by the order queued,
 * as wlan_work_run_due does; 0 when none is due. */
static int run_due(void)
{
    struct wlan_work *w;
    unsigned i, at = 8;

    for (i = 0; i < 8; ++i)
        if (queued[i] && queued[i]->due <= now &&
            (at == 8 || queued[i]->due < queued[at]->due ||
             (queued[i]->due == queued[at]->due && order[i] < order[at])))
            at = i;
    if (at == 8)
        return 0;
    w = queued[at];
    queued[at] = 0;
    w->pending = 0;
    running = w;
    w->run(w);
    running = 0;
    return 1;
}

/** Beacons go to a BSS table this test does not keep. */
int wlan_bss_rx(struct wlan_bss_table *table, const struct wlan_band *band, const uint8_t *frame,
                unsigned len, const struct wlan_rx_status *status, uint64_t now_tick)
{
    (void)table;
    (void)band;
    (void)frame;
    (void)len;
    (void)status;
    (void)now_tick;
    return 0;
}

/** Drop a log line; the rows here judge the frames. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Received Ethernet frames go nowhere; the test sends only. */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    (void)nd;
    (void)data;
    (void)len;
}

/* --- The stub radio: a ring of RING frames, which the test completes. --- */

static struct wlan_radio radio;
static struct wlan wlan;
static struct wlan_bss bss;

struct slot {
    uint64_t cookie;
    unsigned mark, seq;
};

static struct slot ring[RING];
static unsigned ring_count;
static unsigned marks[64]; /* the marks of the frames the radio took, in order */
static unsigned seqs[64];  /* and their sequence numbers */
static int slots[64];      /* and the key slots they went under */
static int takes_keys;     /* set_key puts keys in slots */
static unsigned next_slot;
static unsigned n_taken, refusals;
static unsigned fc_seen;              /* the frame control of the frame last taken */
static unsigned refuse_mark = 0xffff; /* a frame refused with -EIO */
static unsigned refuse_next;          /* how many frames more are */

/** Take a frame while the ring has room, as ath9k_tx does: -ENOBUFS when
 * it is full, and -EIO for the frame the test marks bad. */
static int radio_tx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                    const struct wlan_tx_info *info)
{
    unsigned at = MPDU_MARK + (frame[1] & 0x40 ? WLAN_CCMP_HDR_LEN : 0);
    unsigned mark = len > at ? frame[at] : 0xffff;

    (void)radio;
    fc_seen = (unsigned)frame[0] | (unsigned)frame[1] << 8;
    if (mark == refuse_mark)
        return -EIO;
    if (refuse_next) {
        --refuse_next;
        return -EIO;
    }
    if (ring_count == RING) {
        ++refusals;
        return -ENOBUFS;
    }
    ring[ring_count].cookie = info->cookie;
    ring[ring_count].mark = mark;
    ring[ring_count].seq = ((unsigned)frame[22] | (unsigned)frame[23] << 8) >> 4;
    ++ring_count;
    if (n_taken < 64) {
        marks[n_taken] = mark;
        slots[n_taken] = info->flags & WLAN_TX_HW_KEY ? info->hw_key : -1;
        seqs[n_taken] = ring[ring_count - 1].seq;
    }
    ++n_taken;
    return 0;
}

/** Report the oldest `n` frames of the ring, acknowledged or failed, from
 * what stands for the interrupt handler. */
static void complete(unsigned n, int acked)
{
    struct wlan_tx_status st;
    unsigned i;

    while (n-- && ring_count) {
        k_memset(&st, 0, sizeof(st));
        st.cookie = ring[0].cookie;
        st.flags = acked ? WLAN_TX_ACKED : 0;
        for (i = 0; i < WLAN_TX_RATE_SERIES; ++i)
            st.rates[i].index = -1;
        st.rates[0].index = 0;
        st.rates[0].count = 1;
        for (i = 1; i < ring_count; ++i)
            ring[i - 1] = ring[i];
        --ring_count;
        wlan_radio_tx_status(&radio, &st);
    }
}

static int radio_start(struct wlan_radio *radio)
{
    (void)radio;
    return 0;
}

static void radio_stop(struct wlan_radio *r);

static int radio_config(struct wlan_radio *radio, const struct wlan_channel *channel,
                        unsigned filters)
{
    (void)radio;
    (void)channel;
    (void)filters;
    return 0;
}

/** Give each key the next slot when takes_keys is set; removing is free. */
static int radio_set_key(struct wlan_radio *radio, int enable, const uint8_t peer[6],
                         const struct wlan_key *key, int *slot)
{
    (void)radio;
    (void)peer;
    (void)key;
    if (!takes_keys)
        return -EOPNOTSUPP;
    if (enable)
        *slot = (int)next_slot++;
    return 0;
}

static const struct wlan_radio_ops ops = {
    radio_start, radio_stop, radio_config, radio_tx, radio_set_key, 0, 0, 0, 0, 0};

/** Fail what the ring holds, as a stop must. */
static void radio_stop(struct wlan_radio *r)
{
    struct wlan_tx_status st;
    unsigned i;

    (void)r;
    while (ring_count) {
        k_memset(&st, 0, sizeof(st));
        st.cookie = ring[0].cookie;
        for (i = 0; i < WLAN_TX_RATE_SERIES; ++i)
            st.rates[i].index = -1;
        for (i = 1; i < ring_count; ++i)
            ring[i - 1] = ring[i];
        --ring_count;
        wlan_radio_tx_status(&radio, &st);
    }
}

/** Queue Ethernet frame number `mark` of EtherType `type` through the
 * adapter's tx_ethernet. */
static int send_type(unsigned mark, unsigned type)
{
    uint8_t eth[ETH_HDR_SIZE + 40];

    k_memset(eth, 0, sizeof(eth));
    k_memcpy(eth, far, 6);
    k_memcpy(eth + 6, sta, 6);
    eth[12] = (uint8_t)(type >> 8);
    eth[13] = (uint8_t)type;
    eth[14] = 0x45;
    eth[ETH_MARK] = (uint8_t)mark;
    return wlan.tx_ethernet(&wlan, eth, sizeof(eth));
}

/** Queue IPv4 frame number `mark`, best effort. */
static int send(unsigned mark)
{
    return send_type(mark, 0x0800);
}

/** Queue frame number `mark` over IP as protocol `proto` with `data` bytes
 * after a 20-byte header whose flags byte is `flags`, IPv6 if `v6`, padded to
 * Ethernet's least. */
static int send_flags(unsigned mark, unsigned proto, unsigned data, int v6, unsigned flags)
{
    uint8_t eth[ETH_HDR_SIZE + 128];
    unsigned ip = v6 ? 40 : 20;
    unsigned len = ETH_HDR_SIZE + ip + 20 + data;

    k_memset(eth, 0, sizeof(eth));
    k_memcpy(eth, far, 6);
    k_memcpy(eth + 6, sta, 6);
    if (v6) {
        eth[12] = 0x86;
        eth[13] = 0xdd;
        eth[14] = 0x60;
        eth[19] = (uint8_t)(20 + data);
        eth[20] = (uint8_t)proto;
    } else {
        eth[12] = 0x08;
        eth[14] = 0x45;
        eth[17] = (uint8_t)(ip + 20 + data);
        eth[23] = (uint8_t)proto;
    }
    eth[ETH_HDR_SIZE + ip + 12] = 0x50; /* TCP's data offset: 20 bytes */
    eth[ETH_HDR_SIZE + ip + 13] = (uint8_t)flags;
    eth[ETH_MARK] = (uint8_t)mark;
    return wlan.tx_ethernet(&wlan, eth, len < 60 ? 60 : len);
}

/** Queue frame number `mark` over IP with an acknowledgement's flags. */
static int send_ip(unsigned mark, unsigned proto, unsigned data, int v6)
{
    return send_flags(mark, proto, data, v6, 0x10);
}

/** Queue TCP data, frame number `mark`. */
static int send_tcp(unsigned mark)
{
    return send_ip(mark, 6, 64, 0);
}

/** The frames the adapter's queue holds. */
static unsigned waiting(void)
{
    return wlan_data_of(&wlan)->txq_count;
}

/** Register nothing but what the data path reads: an authorized station
 * on an open BSS without QoS, its radio running and its data path
 * connected. */
static void setup(void)
{
    static const uint16_t rates[4] = {10, 20, 55, 110};
    static const uint8_t ies[] = {0, 4, 'w', 'i', 'f', 'i', 1, 4, 0x82, 0x84, 0x8b, 0x96, 3, 1, 1};
    unsigned i;

    k_memset(&radio, 0, sizeof(radio));
    radio.ops = &ops;
    radio.name = "stub";
    k_memcpy(radio.mac, sta, 6);
    for (i = 0; i < 4; ++i)
        radio.band.rates[i] = rates[i];
    radio.band.n_rates = 4;
    radio.band.n_channels = 1;
    radio.band.channels[0].frequency = 2412;
    radio.band.channels[0].number = 1;
    k_memset(&wlan, 0, sizeof(wlan));
    wlan.radio = &radio;
    radio.wlan = &wlan;
    wlan.netdev.up = 1;
    wlan.started = 1;
    k_memset(&bss, 0, sizeof(bss));
    k_memcpy(bss.bssid, bssid, 6);
    bss.frequency = 2412;
    bss.capability = WLAN_CAPABILITY_ESS;
    k_memcpy(bss.ies, ies, sizeof(ies));
    bss.ies_len = sizeof(ies);
    CHECK(wlan_data_attach(&wlan, 0) == 0);
    wlan.state = WLAN_STA_AUTHORIZED;
    wlan.bss = &bss;
    wlan_data_state(&wlan, &bss, WLAN_STA_NONE, WLAN_STA_AUTHORIZED);
    CHECK(wlan_data_of(&wlan)->connected);
    ring_count = n_taken = refusals = 0;
    refuse_mark = 0xffff;
    takes_keys = 0;
    next_slot = 0;
}

/* --- The checks. --- */

/**
 * Frames into a ring of four behind the slots of the queue that data
 * other than TCP's may take: the ring fills, the queue holds the rest
 * without a drain queued per frame, the next is refused to its sender,
 * and completions one at a time send them all in order with consecutive
 * sequence numbers.
 */
static void check_hold(void)
{
    unsigned i, data = WLAN_DATA_TX_QUEUE - WLAN_DATA_TX_EAPOL, total = RING + data;

    setup();
    for (i = 0; i < data; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(ring_count == RING);
    CHECK(n_taken == RING);
    /* The first two frames queued both drain records, and each run stops
     * at its first refusal. */
    CHECK(refusals >= 1 && refusals <= 2);
    CHECK(waiting() == data - RING);
    CHECK(wlan_data_of(&wlan)->txq_held);
    CHECK(wlan.netdev.tx_dropped == 0);

    /* A held queue takes frames without queuing a drain that would only
     * be refused again. */
    for (; i < total; ++i)
        CHECK(send(i) == 0);
    CHECK(pending() == 0);
    CHECK(waiting() == data);
    CHECK(send(total) == -ENOBUFS);

    /* Each completion frees a slot, which the next frame takes. */
    while (ring_count) {
        complete(1, 1);
        run_work();
    }
    CHECK(n_taken == total);
    for (i = 0; i < total && i < 64; ++i) {
        CHECK(marks[i] == i);
        CHECK(seqs[i] == i);
    }
    CHECK(waiting() == 0);
    CHECK(!wlan_data_of(&wlan)->txq_held);
    CHECK(wlan.netdev.tx_dropped == 0);
    CHECK(wlan.counters.tx_packets == total);
    CHECK(wlan.counters.tx_failed == 0);

    /* With room again, a frame goes at once. */
    CHECK(send(99) == 0);
    run_work();
    CHECK(n_taken == total + 1 && marks[total] == 99);
    complete(1, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/**
 * EAPOL behind a bulk transfer, as a group rekey's reply comes while the
 * queue is full of data: the last two slots take EAPOL alone, a third
 * EAPOL frame is refused, and the two go after the data queued before
 * them.
 */
static void check_eapol_room(void)
{
    unsigned i, data = WLAN_DATA_TX_QUEUE - WLAN_DATA_TX_EAPOL, total = RING + data;

    setup();
    for (i = 0; i < data; ++i)
        CHECK(send(i) == 0);
    run_work();
    for (; i < total; ++i)
        CHECK(send(i) == 0);
    CHECK(waiting() == data);
    CHECK(send(total) == -ENOBUFS);
    CHECK(send_type(100, 0x888e) == 0);
    CHECK(send_type(101, 0x888e) == 0);
    CHECK(waiting() == WLAN_DATA_TX_QUEUE);
    CHECK(send_type(102, 0x888e) == -ENOBUFS);
    CHECK(pending() == 0);

    while (ring_count) {
        complete(1, 1);
        run_work();
    }
    CHECK(n_taken == total + 2);
    for (i = 0; i < total; ++i)
        CHECK(marks[i] == i);
    CHECK(marks[total] == 100 && marks[total + 1] == 101);
    CHECK(waiting() == 0);
    CHECK(wlan.netdev.tx_dropped == 0);
    wlan_data_detach(&wlan);
}

/**
 * Other traffic behind a bulk upload: TCP data stops at its fourteen slots,
 * as many as it had before the others were added, and is refused,
 * counted as TCP's. ICMP, UDP, ARP and a bare acknowledgement, on IPv4 and
 * IPv6, go in behind it, and a padded acknowledgement is no data. Only when
 * those slots are full is one refused, counted as other, and EAPOL then has
 * its own two. All go in order.
 */
static void check_upload_room(void)
{
    unsigned others = WLAN_DATA_TX_OTHER + WLAN_DATA_TX_ACK;
    unsigned i, want, tcp = 14, total = RING + tcp;

    setup();
    CHECK(WLAN_DATA_TX_QUEUE - WLAN_DATA_TX_EAPOL - others == tcp);
    for (i = 0; i < tcp; ++i)
        CHECK(send_tcp(i) == 0);
    run_work();
    for (; i < total; ++i)
        CHECK(send_tcp(i) == 0);
    CHECK(waiting() == tcp);
    CHECK(send_tcp(total) == -ENOBUFS);
    CHECK(send_ip(total, 6, 64, 1) == -ENOBUFS);
    CHECK(wlan.counters.tx_refused_tcp == 2);
    CHECK(wlan.counters.tx_refused_other == 0);

    CHECK(send_ip(100, 1, 16, 0) == 0);  /* ICMP */
    CHECK(send_ip(101, 17, 16, 0) == 0); /* UDP */
    CHECK(send_ip(102, 6, 0, 0) == 0);   /* a bare acknowledgement */
    CHECK(send_ip(103, 6, 0, 1) == 0);   /* and on IPv6 */
    CHECK(send_ip(104, 58, 16, 1) == 0); /* ICMPv6 */
    CHECK(send_ip(105, 17, 16, 1) == 0); /* UDP on IPv6 */
    CHECK(send_type(106, 0x0806) == 0);  /* ARP */
    CHECK(send(107) == 0);
    CHECK(waiting() == tcp + others);
    CHECK(send(108) == -ENOBUFS);
    CHECK(wlan.counters.tx_refused_other == 1);
    CHECK(wlan.counters.tx_refused_tcp == 2);

    CHECK(send_type(109, 0x888e) == 0);
    CHECK(send_type(110, 0x888e) == 0);
    CHECK(send_type(111, 0x888e) == -ENOBUFS);
    CHECK(wlan.counters.tx_refused_other == 2);
    CHECK(waiting() == WLAN_DATA_TX_QUEUE);

    while (ring_count) {
        complete(1, 1);
        run_work();
    }
    want = total + others + WLAN_DATA_TX_EAPOL;
    CHECK(n_taken == want);
    for (i = 0; i < total; ++i)
        CHECK(marks[i] == i);
    for (i = 0; i < others + WLAN_DATA_TX_EAPOL; ++i)
        CHECK(marks[total + i] == 100 + i + (i >= others ? 1 : 0));
    CHECK(waiting() == 0);
    wlan_data_detach(&wlan);
}

/**
 * Who stops where behind data and acknowledgements each way. Bare
 * acknowledgements stop at 18, counted as theirs, and leave the frames that
 * nobody sends again room: ICMP, UDP, a SYN, a FIN, a reset, a FIN that
 * carries data, and a one-byte probe, whose sender ignores a refusal. Two
 * bytes are data, on either IP.
 */
static void check_ack_room(void)
{
    unsigned i, ack = 14 + WLAN_DATA_TX_ACK;

    setup();
    for (i = 0; i < RING; ++i)
        CHECK(send_tcp(i) == 0);
    run_work();
    for (i = 0; i < 14; ++i)
        CHECK(send_tcp(i) == 0);
    CHECK(send_ip(20, 6, 2, 0) == -ENOBUFS);
    CHECK(send_ip(21, 6, 2, 1) == -ENOBUFS);
    CHECK(wlan.counters.tx_refused_tcp == 2);
    for (i = 14; i < ack; ++i)
        CHECK(send_ip(30, 6, 0, 0) == 0);
    CHECK(send_ip(31, 6, 0, 0) == -ENOBUFS);
    CHECK(send_ip(32, 6, 0, 1) == -ENOBUFS);
    CHECK(wlan.counters.tx_refused_ack == 2);
    CHECK(wlan.counters.tx_refused_other == 0);
    CHECK(send_ip(33, 1, 16, 0) == 0);         /* ICMP */
    CHECK(send_flags(34, 6, 0, 0, 0x02) == 0); /* a SYN */
    CHECK(send_flags(35, 6, 0, 0, 0x11) == 0); /* a FIN */
    CHECK(send_ip(36, 6, 1, 0) == 0);          /* a probe */
    CHECK(waiting() == WLAN_DATA_TX_QUEUE - WLAN_DATA_TX_EAPOL);
    CHECK(send_flags(37, 6, 0, 0, 0x14) == -ENOBUFS);  /* a reset */
    CHECK(send_flags(38, 6, 64, 0, 0x11) == -ENOBUFS); /* a FIN with data */
    CHECK(wlan.counters.tx_refused_tcp == 2);
    CHECK(wlan.counters.tx_refused_ack == 2);
    CHECK(wlan.counters.tx_refused_other == 2);
    wlan_data_detach(&wlan);
}

/* A received frame's hook: what the test has it do. */
static unsigned hook_calls, hook_taken_at_9;
static int hook_sends, hook_disconnects;

/** The MLME's hook on a management frame: answer it with a frame, as TCP
 * answers a segment, or end the connection, as a deauthentication does. */
static int hook(struct wlan *w, const uint8_t *frame, unsigned len, unsigned flags)
{
    (void)frame;
    (void)len;
    (void)flags;
    if (++hook_calls == 9)
        hook_taken_at_9 = n_taken;
    if (hook_sends)
        (void)send(hook_calls);
    if (hook_disconnects)
        wlan_data_disconnect(wlan_data_of(w));
    return 1;
}

/** Queue a deauthentication from the AP as the radio would. */
static int receive(void)
{
    struct wlan_rx_status st;
    uint8_t f[26];

    k_memset(f, 0, sizeof(f));
    k_memset(&st, 0, sizeof(st));
    f[0] = 0xc0;
    k_memcpy(f + 4, sta, 6);
    k_memcpy(f + 10, bssid, 6);
    k_memcpy(f + 16, bssid, 6);
    return wlan_radio_rx(&radio, f, sizeof(f), &st);
}

/** A scan's stop and a channel switch's end apart, whichever ends first: the
 * drain runs only when neither stops it, and a null frame carries the power
 * management bit it was asked for. */
static void check_stop_reasons(void)
{
    struct wlan_data *d;

    setup();
    d = wlan_data_of(&wlan);
    wlan_data_stop_scan(d, 1);
    wlan_data_stop_tx(d, 1);
    CHECK(send(0) == 0);
    CHECK(wlan_data_waiting(d));
    run_work();
    wlan_data_stop_tx(d, 0);
    run_work();
    CHECK(n_taken == 0 && waiting() == 1);
    wlan_data_stop_tx(d, 1);
    wlan_data_stop_scan(d, 0);
    run_work();
    CHECK(n_taken == 0 && waiting() == 1);
    wlan_data_stop_tx(d, 0);
    run_work();
    CHECK(n_taken == 1 && waiting() == 0 && !wlan_data_waiting(d));
    complete(1, 1);
    run_work();
    refuse_mark = 0xfffe; /* a null frame is too short to carry a mark */
    CHECK(wlan_data_tx_sleep(d, 1) == 0);
    CHECK(fc_seen & IEEE80211_FCTL_PM);
    CHECK(wlan_data_tx_sleep(d, 0) == 0);
    CHECK(!(fc_seen & IEEE80211_FCTL_PM));
    CHECK(wlan_data_tx_null(d, 0) == 0);
    CHECK(!(fc_seen & IEEE80211_FCTL_PM));
    wlan_data_detach(&wlan);
}

/** A power save null frame goes once more when it fails, with its own bit,
 * and says how it ended: told, waiting or failed. A frame replaced by a later
 * one has no say, and nothing is told while the station has no AP. */
static void check_sleep_status(void)
{
    struct wlan_data *d;
    unsigned sent;

    setup();
    d = wlan_data_of(&wlan);
    refuse_mark = 0xfffe;
    CHECK(wlan_data_tx_sleep(d, 1) == 0);
    CHECK(wlan_data_sleep_told(d) == 0);
    complete(1, 1);
    run_work();
    CHECK(wlan_data_sleep_told(d) == 1 && ring_count == 0);

    sent = n_taken;
    CHECK(wlan_data_tx_sleep(d, 1) == 0);
    complete(1, 0);
    run_work();
    CHECK(n_taken == sent + 2 && (fc_seen & IEEE80211_FCTL_PM));
    CHECK(wlan_data_sleep_told(d) == 0);
    complete(1, 1);
    run_work();
    CHECK(wlan_data_sleep_told(d) == 1 && n_taken == sent + 2);

    sent = n_taken;
    CHECK(wlan_data_tx_sleep(d, 0) == 0);
    complete(1, 0);
    run_work();
    CHECK(n_taken == sent + 2 && !(fc_seen & IEEE80211_FCTL_PM));
    complete(1, 0);
    run_work();
    CHECK(wlan_data_sleep_told(d) == -1 && n_taken == sent + 2 && ring_count == 0);

    sent = n_taken;
    CHECK(wlan_data_tx_sleep(d, 1) == 0);
    CHECK(wlan_data_tx_sleep(d, 0) == 0);
    complete(1, 0);
    run_work();
    CHECK(n_taken == sent + 2 && wlan_data_sleep_told(d) == 0);
    complete(1, 1);
    run_work();
    CHECK(wlan_data_sleep_told(d) == 1 && ring_count == 0);

    sent = n_taken;
    refuse_next = 1;
    CHECK(wlan_data_tx_sleep(d, 0) == 0);
    CHECK(n_taken == sent + 1 && wlan_data_sleep_told(d) == 0);
    complete(1, 1);
    run_work();
    CHECK(wlan_data_sleep_told(d) == 1);

    refuse_next = 2;
    CHECK(wlan_data_tx_sleep(d, 1) == -EIO);
    CHECK(wlan_data_sleep_told(d) == -1 && refuse_next == 0 && ring_count == 0);

    d->connected = 0;
    CHECK(wlan_data_tx_sleep(d, 1) == -ENOTCONN);
    CHECK(wlan_data_sleep_told(d) == 1);
    d->connected = 1;
    wlan_data_detach(&wlan);
}

/** Answers wait for no end to a stream of received frames: the worker sends
 * what is queued every WLAN_DATA_RX_BATCH frames. */
static void check_rx_sends(void)
{
    unsigned i;

    setup();
    wlan_data_of(&wlan)->rx_mgmt = hook;
    hook_calls = hook_taken_at_9 = 0;
    hook_sends = 1;
    hook_disconnects = 0;
    for (i = 0; i < 2 * WLAN_DATA_RX_BATCH + 1; ++i)
        CHECK(receive() == 0);
    run_work();
    CHECK(hook_calls == 2 * WLAN_DATA_RX_BATCH + 1);
    CHECK(hook_taken_at_9 >= 1);
    hook_sends = 0;
    wlan_data_detach(&wlan);
}

/** The receive queue holds a pass of both of ath9k's FIFOs, says when it is
 * full, as a radio asks before it reads a frame, and times the worker. */
static void check_rx_full(void)
{
    unsigned i;

    setup();
    wlan_data_of(&wlan)->rx_mgmt = hook;
    hook_calls = 0;
    hook_sends = hook_disconnects = 0;
    CHECK(WLAN_DATA_RX_QUEUE >= 128 + 16);
    for (i = 0; i < WLAN_DATA_RX_QUEUE; ++i) {
        CHECK(!wlan_radio_rx_full(&radio));
        CHECK(receive() == 0);
    }
    CHECK(wlan.counters.rx_held == 0);
    CHECK(wlan_radio_rx_full(&radio));
    CHECK(wlan.counters.rx_held == 1);
    CHECK(wlan.counters.rx_dropped == 0);
    now += 7;
    run_work();
    CHECK(hook_calls == WLAN_DATA_RX_QUEUE);
    CHECK(wlan.counters.rx_wait_most == 7);
    CHECK(!wlan_radio_rx_full(&radio));
    wlan_data_detach(&wlan);
}

/* The flood of check_rx_bound: frames taken, frames the radio queued, and
 * how many were taken when the timer ran. */
static unsigned flood_taken, flood_queued, flood_at_timer;

/** The hook on each flooded frame: the radio has the next one ready by the
 * time the worker takes this one, and a tick passes. */
static int flood(struct wlan *w, const uint8_t *frame, unsigned len, unsigned flags)
{
    (void)w;
    (void)frame;
    (void)len;
    (void)flags;
    ++flood_taken;
    if (flood_queued < 500) {
        CHECK(receive() == 0);
        ++flood_queued;
    }
    ++now;
    return 1;
}

/** A timer of the worker's, which notes how far the flood had gone. */
static void flood_timer(struct wlan_work *w)
{
    (void)w;
    flood_at_timer = flood_taken;
}

/**
 * A steady stream holds up no timer: the radio queues one frame for each the
 * worker takes, a tick for each, until 500, and a timer comes due during the
 * first call of the receive work. It runs before the stream ends, behind no
 * more than a budget of frames past its deadline, and every frame is taken.
 */
static void check_rx_bound(void)
{
    struct wlan_work timer;

    setup();
    now = 0;
    wlan_data_of(&wlan)->rx_mgmt = flood;
    flood_taken = flood_at_timer = 0;
    k_memset(&timer, 0, sizeof(timer));
    timer.run = flood_timer;
    CHECK(wlan_queue_work(&timer, 10) == 0);
    CHECK(receive() == 0);
    flood_queued = 1;
    while (run_due())
        ;
    CHECK(flood_taken == 500 && !timer.pending);
    CHECK(flood_at_timer >= 10 && flood_at_timer <= 10 + WLAN_DATA_RX_BUDGET);
    CHECK(pending() == 0);
    now = 0;
    wlan_data_detach(&wlan);
}

/* What the data path passed on: frames' statuses by cookie, and the attempts
 * of reports of tries alone as rate control charges them. */
static uint8_t status_seen[256];
static unsigned statuses, tries_reports, tries_attempts, tries_mpdus;

/** Rate control's status: sum what a report of tries alone charges. */
static void rate_status(struct wlan_radio *r, const struct wlan_tx_status *st)
{
    unsigned i, mpdus = st->ampdu_len ? st->ampdu_len : 1;

    (void)r;
    if (!(st->flags & WLAN_TX_NO_FRAME))
        return;
    ++tries_reports;
    tries_mpdus += mpdus;
    for (i = 0; i < WLAN_TX_RATE_SERIES && st->rates[i].index >= 0; ++i)
        tries_attempts += st->rates[i].count * mpdus;
}

static const struct wlan_rate_ops rate_ops = {0, rate_status};

/** The station's hook on every completion: note each frame's. */
static void status_hook(struct wlan *w, const struct wlan_tx_status *st)
{
    (void)w;
    if (st->flags & WLAN_TX_NO_FRAME)
        return;
    ++statuses;
    if ((st->cookie & 0xff) < 256 && (st->cookie & WLAN_DATA_COOKIE))
        status_seen[st->cookie & 0xff]++;
}

/** Report frame `n`'s status, failed, as ath9k's interrupt handler does. */
static void frame_status(unsigned n)
{
    struct wlan_tx_status st;
    unsigned i;

    k_memset(&st, 0, sizeof(st));
    st.cookie = WLAN_DATA_COOKIE | n;
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i)
        st.rates[i].index = -1;
    st.rates[0].index = 0;
    st.rates[0].count = 1;
    wlan_radio_tx_status(&radio, &st);
}

/** Report a push of `mpdus` that failed whole, every frame kept for another
 * try, at `rate` four times and the lowest rate twice, as
 * ath9k_tx_report_tries does. */
static void push_failed(unsigned mpdus, int rate)
{
    struct wlan_tx_status st;
    unsigned i;

    k_memset(&st, 0, sizeof(st));
    st.flags = WLAN_TX_NO_FRAME;
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i)
        st.rates[i].index = -1;
    st.rates[0].index = (int16_t)rate;
    st.rates[0].count = 4;
    st.rates[0].flags = WLAN_RATE_MCS;
    st.rates[1].index = 0;
    st.rates[1].count = 2;
    st.ampdu_len = (uint8_t)mpdus;
    wlan_radio_tx_status(&radio, &st);
}

/**
 * Every frame's status arrives though the worker was away while the radio's
 * four rings of 64 emptied, behind a push that failed whole: the report of
 * its tries takes no frame's place, and rate control sees them. Reports of
 * tries alone past their own queue are merged with exact attempts, and one
 * with nothing to merge with is counted lost.
 */
static void check_status_full(void)
{
    struct wlan_data *d;
    unsigned i, ok = 1;

    setup();
    d = wlan_data_of(&wlan);
    radio.rate_ops = &rate_ops;
    d->tx_status = status_hook;
    k_memset(status_seen, 0, sizeof(status_seen));
    statuses = tries_reports = tries_attempts = tries_mpdus = 0;
    push_failed(20, 7);
    for (i = 0; i < 4 * 64; ++i)
        frame_status(i);
    run_work();
    for (i = 0; i < 256; ++i)
        ok &= status_seen[i] == 1;
    CHECK(statuses == 256 && ok);
    CHECK(tries_reports == 1 && tries_mpdus == 20 && tries_attempts == 6 * 20);
    CHECK(wlan.counters.tx_failed == 256 && wlan.counters.tx_status_lost == 0);

    statuses = tries_reports = tries_attempts = tries_mpdus = 0;
    for (i = 0; i < WLAN_DATA_TRIES_QUEUE + 3; ++i)
        push_failed(10, 7);
    push_failed(10, 5);
    run_work();
    CHECK(tries_reports == WLAN_DATA_TRIES_QUEUE);
    CHECK(tries_mpdus == 10 * (WLAN_DATA_TRIES_QUEUE + 3));
    CHECK(tries_attempts == 6 * 10 * (WLAN_DATA_TRIES_QUEUE + 3));
    CHECK(wlan.counters.tx_status_lost == 1);
    radio.rate_ops = 0;
    wlan_data_detach(&wlan);
}

/** A refusal other than a full ring drops that frame, counted, and the
 * frames behind it go. */
static void check_other_refusal(void)
{
    setup();
    refuse_mark = 1;
    CHECK(send(0) == 0);
    CHECK(send(1) == 0);
    CHECK(send(2) == 0);
    run_work();
    CHECK(n_taken == 2 && marks[0] == 0 && marks[1] == 2);
    CHECK(wlan.netdev.tx_dropped == 1);
    CHECK(waiting() == 0);
    CHECK(!wlan_data_of(&wlan)->txq_held);
    complete(2, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/**
 * A stop while frames wait behind a full ring, in wlan_stop's order: the
 * data path disconnects, which empties the queue and frees it, and the
 * radio then fails what its ring held. Nothing more is sent, and the
 * adapter takes no frame until the station connects again.
 */
static void check_stop(void)
{
    unsigned i;

    setup();
    for (i = 0; i < 10; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(ring_count == RING && waiting() == 10 - RING);
    CHECK(wlan_data_of(&wlan)->txq_held);

    wlan.state = WLAN_STA_NONE;
    wlan.bss = 0;
    wlan_data_disconnect(wlan_data_of(&wlan));
    CHECK(waiting() == 0);
    CHECK(!wlan_data_of(&wlan)->txq_held);
    wlan.started = 0;
    radio_stop(&radio);
    run_work();
    CHECK(n_taken == RING);
    CHECK(wlan.counters.tx_failed == RING);
    CHECK(send(20) == -ENOTCONN);
    run_work();
    CHECK(n_taken == RING);
    wlan_data_detach(&wlan);
}

/** Install the pairwise key of the AP whose first byte is `fill` through
 * the interface's path. */
static int pairwise(unsigned fill)
{
    struct wlan_key k;

    k_memset(&k, 0, sizeof(k));
    k.cipher = WLAN_CIPHER_SUITE_CCMP;
    k.flags = WLAN_KEY_PAIRWISE;
    k.len = 16;
    k.hw_slot = -1;
    k_memset(k.data, (int)fill, 16);
    k_memcpy(k.peer, bssid, 6);
    return wlan_install_key(&wlan, &k);
}

/** A protected station whose key took slot 0. */
static void setup_keyed(void)
{
    setup();
    takes_keys = 1;
    radio.hw_ciphers = WLAN_CIPHER_HW_CCMP;
    wlan_data_of(&wlan)->privacy = 1;
    CHECK(pairwise(1) == 0);
    CHECK(wlan_data_of(&wlan)->pairwise[0].key.hw_slot == 0);
}

/**
 * A key renewal behind a full ring: message 4 waits in the queue with data
 * before it, the key waits for both, what comes after the key stays for the
 * new key, and the install ends once they have gone.
 */
static void check_rekey_ring(void)
{
    unsigned i;

    setup_keyed();
    for (i = 0; i < RING + 1; ++i)
        CHECK(send(i) == 0);
    CHECK(send_type(50, 0x888e) == 0);
    run_work();
    CHECK(ring_count == RING && waiting() == 2);
    CHECK(pairwise(2) == -EINPROGRESS);
    CHECK(wlan_data_of(&wlan)->pairwise[0].key.hw_slot == 0);
    CHECK(send(60) == 0);
    complete(1, 1);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    complete(1, 1);
    run_work();
    CHECK(n_taken == RING + 2 && marks[RING + 1] == 50 && slots[RING + 1] == 0);
    CHECK(waiting() == 1);
    CHECK(pairwise(2) == 0);
    CHECK(wlan_data_of(&wlan)->pairwise[0].key.hw_slot == 1);
    complete(1, 1);
    run_work();
    CHECK(n_taken == RING + 3 && marks[RING + 2] == 60 && slots[RING + 2] == 1);
    wlan_data_detach(&wlan);
}

/** A channel switch's hold keeps the queue, and the key waits for it. */
static void check_rekey_hold(void)
{
    setup_keyed();
    wlan_data_stop_tx(wlan_data_of(&wlan), 1);
    CHECK(send_type(50, 0x888e) == 0);
    run_work();
    CHECK(waiting() == 1);
    CHECK(pairwise(2) == -EINPROGRESS);
    wlan_data_stop_tx(wlan_data_of(&wlan), 0);
    run_work();
    CHECK(n_taken == 1 && marks[0] == 50 && slots[0] == 0);
    CHECK(pairwise(2) == 0);
    wlan_data_detach(&wlan);
}

/** Data with no EAPOL frame behind it holds no key up, and goes under the
 * new one, since a frame under the old key can cross the AP's change. */
static void check_rekey_bound(void)
{
    unsigned i;

    setup_keyed();
    for (i = 0; i < RING + 1; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(waiting() == 1);
    CHECK(pairwise(2) == 0);
    CHECK(wlan_data_of(&wlan)->pairwise[0].key.hw_slot == 1);
    complete(RING, 1);
    run_work();
    CHECK(n_taken == RING + 1 && marks[RING] == RING && slots[RING] == 1);
    wlan_data_detach(&wlan);
}

/** Message 4 still queued past the wait keeps the old key: under the new
 * one the AP could not read it, and the link would look up with no renewal
 * done. The install fails, for wpa_supplicant to join again. */
static void check_rekey_eapol_full_ring(void)
{
    unsigned i;

    setup_keyed();
    for (i = 0; i < RING; ++i)
        CHECK(send(i) == 0);
    CHECK(send_type(RING, 0x888e) == 0);
    run_work();
    CHECK(wlan_data_eapol_queued(wlan_data_of(&wlan)));
    CHECK(pairwise(2) == -EINPROGRESS);
    now += WLAN_DATA_REKEY_WAIT;
    CHECK(pairwise(2) == -ETIMEDOUT);
    CHECK(wlan_data_of(&wlan)->pairwise[0].key.hw_slot == 0);
    CHECK(!wlan_data_of(&wlan)->install_wait);
    complete(RING, 1);
    run_work();
    CHECK(n_taken == RING + 1 && marks[RING] == RING && slots[RING] == 0);
    CHECK(!wlan_data_eapol_queued(wlan_data_of(&wlan)));
    wlan_data_detach(&wlan);
}

/** A channel switch of mode 1 that outlasts the wait keeps message 4 queued
 * the same way, with the ring empty. */
static void check_rekey_eapol_stopped(void)
{
    struct wlan_data *d;

    setup_keyed();
    d = wlan_data_of(&wlan);
    wlan_data_stop_tx(d, 1);
    CHECK(send_type(0, 0x888e) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    now += WLAN_DATA_REKEY_WAIT;
    CHECK(pairwise(2) == -ETIMEDOUT);
    CHECK(d->pairwise[0].key.hw_slot == 0);
    wlan_data_stop_tx(d, 0);
    run_work();
    CHECK(n_taken == 1 && marks[0] == 0 && slots[0] == 0);
    wlan_data_detach(&wlan);
}

/** A deauthentication among the frames the first try takes ends the install:
 * the key must not outlive the connection, or the next one's EAPOL goes under
 * it. */
static void check_rekey_lost_first(void)
{
    setup_keyed();
    wlan_data_of(&wlan)->rx_mgmt = hook;
    hook_calls = 0;
    hook_sends = 0;
    hook_disconnects = 1;
    CHECK(receive() == 0);
    CHECK(pairwise(2) == -ENOTCONN);
    CHECK(hook_calls == 1);
    CHECK(!wlan_data_of(&wlan)->pairwise[0].gen);
    hook_disconnects = 0;
    run_work();
    wlan_data_detach(&wlan);
}

/** A connection lost during the wait ends the install with -ENOTCONN. */
static void check_rekey_lost(void)
{
    unsigned i;

    setup_keyed();
    for (i = 0; i < RING; ++i)
        CHECK(send(i) == 0);
    CHECK(send_type(50, 0x888e) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    wlan_data_disconnect(wlan_data_of(&wlan));
    CHECK(pairwise(2) == -ENOTCONN);
    CHECK(!wlan_data_of(&wlan)->pairwise[0].gen);
    complete(RING, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/** One install has one wait for the queue: what is queued during a DELBA's
 * wait is not waited for again. */
static void check_rekey_one_wait(void)
{
    unsigned i;

    setup_keyed();
    wlan_data_of(&wlan)->ba_tx[0].state = WLAN_BA_TX_ACTIVE;
    refuse_mark = 0xfffe;
    for (i = 0; i < RING; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    CHECK(send(RING) == 0);
    CHECK(pairwise(2) == -EINPROGRESS);
    CHECK(wlan_data_of(&wlan)->rekey_due == 0);
    complete(RING, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/** A connection that ends in a key's wait leaves no wait behind for the next
 * one, whose frames, message 2 among them, go out. */
static void check_rekey_gone(void)
{
    unsigned i;

    setup_keyed();
    for (i = 0; i < RING; ++i)
        CHECK(send(i) == 0);
    CHECK(send_type(50, 0x888e) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    wlan_data_disconnect(wlan_data_of(&wlan));
    CHECK(wlan_data_of(&wlan)->rekey_due == 0);
    complete(RING, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/** A connection lost while a DELBA waits for room in the ring ends the
 * install with -ENOTCONN too, after the queue's wait has ended. */
static void check_rekey_lost_delba(void)
{
    unsigned i;

    setup_keyed();
    wlan_data_of(&wlan)->ba_tx[0].state = WLAN_BA_TX_ACTIVE;
    refuse_mark = 0xfffe; /* a short frame reads as 0xffff */
    for (i = 0; i < RING; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    CHECK(wlan_data_of(&wlan)->rekey_due == 0);
    wlan_data_disconnect(wlan_data_of(&wlan));
    CHECK(pairwise(2) == -ENOTCONN);
    CHECK(!wlan_data_of(&wlan)->pairwise[0].gen);
    complete(RING, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/**
 * A key held up by a DELBA that cannot queue, its marked frames gone: what
 * came after them stays queued for the key, though the ring frees a slot,
 * and goes under it once the key is in.
 */
static void check_rekey_delba_hold(void)
{
    unsigned i, n;

    setup_keyed();
    wlan_data_of(&wlan)->ba_tx[0].state = WLAN_BA_TX_ACTIVE;
    refuse_mark = 0xfffe; /* a short frame reads as 0xffff */
    for (i = 0; i < RING; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    n = n_taken;
    CHECK(send(20) == 0);
    complete(1, 1);
    run_work();
    CHECK(n_taken == n && waiting() == 1);
    complete(2, 1);
    CHECK(pairwise(2) == 0);
    run_work();
    CHECK(waiting() == 0 && marks[n_taken - 1] == 20);
    complete(RING - 1, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/**
 * A new key ends a transmit session at its first call, ahead of the wait for
 * message 4, since a session's retries are frames of the old key; the same
 * key again, as a replayed message 3 gives it, ends nothing.
 */
static void check_rekey_delba_first(void)
{
    setup_keyed();
    wlan_data_of(&wlan)->ba_tx[0].state = WLAN_BA_TX_ACTIVE;
    wlan_data_stop_tx(wlan_data_of(&wlan), 1);
    CHECK(send_type(50, 0x888e) == 0);
    run_work();
    CHECK(pairwise(1) == -EINPROGRESS);
    CHECK(wlan_data_of(&wlan)->ba_tx[0].state == WLAN_BA_TX_ACTIVE);
    wlan_data_stop_tx(wlan_data_of(&wlan), 0);
    run_work();
    CHECK(pairwise(1) == 0);
    CHECK(wlan_data_of(&wlan)->ba_tx[0].state == WLAN_BA_TX_ACTIVE);
    wlan_data_stop_tx(wlan_data_of(&wlan), 1);
    CHECK(send_type(51, 0x888e) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    CHECK(wlan_data_of(&wlan)->ba_tx[0].state == WLAN_BA_TX_IDLE);
    wlan_data_stop_tx(wlan_data_of(&wlan), 0);
    run_work();
    CHECK(pairwise(2) == 0);
    wlan_data_detach(&wlan);
}

/** Run what is due now, and leave what is not. */
static void run_due_all(void)
{
    while (run_due())
        ;
}

/** Build the AP's ADDBA response of TID 0 to token `token` in `f`, with
 * status 0. Returns its length. */
static unsigned addba_resp(uint8_t *f, unsigned token)
{
    unsigned params = 0x0002 | 64 << 6;

    k_memset(f, 0, 33);
    f[0] = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION;
    k_memcpy(f + 4, sta, 6);
    k_memcpy(f + 10, bssid, 6);
    k_memcpy(f + 16, bssid, 6);
    f[24] = WLAN_CATEGORY_BACK;
    f[25] = WLAN_ACTION_ADDBA_RESP;
    f[26] = (uint8_t)token;
    f[29] = (uint8_t)params;
    f[30] = (uint8_t)(params >> 8);
    return 33;
}

/** An unanswered request's DELBA that finds the ring full goes again, for one
 * more response time, and after that the session stops without it. */
static void check_ba_no_room(void)
{
    struct wlan_data *d;
    unsigned i;

    setup_keyed();
    d = wlan_data_of(&wlan);
    d->ba_tx[0].state = WLAN_BA_TX_ACTIVE;
    refuse_mark = 0xfffe; /* a short frame reads as 0xffff */
    for (i = 0; i < RING; ++i)
        CHECK(send(i) == 0);
    run_work();
    d->ba_tx[0].state = WLAN_BA_TX_REQUESTED;
    d->ba_tx[0].since = now - WLAN_BA_TX_RESPONSE;
    CHECK(wlan_ba_tx_waits(d, 0) == now + WLAN_BA_TX_AGAIN);
    CHECK(d->ba_tx[0].state == WLAN_BA_TX_REQUESTED);
    now += WLAN_BA_TX_RESPONSE;
    CHECK(wlan_ba_tx_waits(d, 0) == 0 && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
    complete(RING, 1);
    run_work();
    wlan_data_detach(&wlan);
}

/**
 * Frames of a TID whose ADDBA request is out wait at the head of the queue,
 * with their numbers unspent, and those behind them; the AP's answer sends
 * them, and with no answer a DELBA stops the request at WLAN_BA_TX_RESPONSE
 * and they follow it. A frame sent ahead of the session would lie below the
 * first number of the AP's window.
 */
static void check_ba_request_holds(void)
{
    struct wlan_data *d;
    uint8_t f[40];
    unsigned len;

    setup();
    d = wlan_data_of(&wlan);
    d->ba_tx[0].state = WLAN_BA_TX_REQUESTED;
    d->ba_tx[0].token = 9;
    d->ba_tx[0].since = now;
    CHECK(send(1) == 0 && send(2) == 0);
    run_due_all();
    CHECK(n_taken == 0 && waiting() == 2);
    len = addba_resp(f, 9);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    run_due_all();
    CHECK(d->ba_tx[0].state == WLAN_BA_TX_IDLE);
    CHECK(n_taken == 2 && marks[0] == 1 && waiting() == 0);
    complete(2, 1);

    d->ba_tx[0].state = WLAN_BA_TX_REQUESTED;
    d->ba_tx[0].since = now;
    CHECK(send(3) == 0);
    run_due_all();
    CHECK(n_taken == 2 && waiting() == 1);
    now += WLAN_BA_TX_RESPONSE - 1;
    run_due_all();
    CHECK(n_taken == 2);
    now += 1;
    run_due_all();
    CHECK(n_taken == 3 && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
    complete(1, 1);
    run_due_all();

    /* A pairwise key on its way waits for nothing. */
    d->ba_tx[0].state = WLAN_BA_TX_REQUESTED;
    d->ba_tx[0].since = now;
    d->rekey_due = now + 1000;
    CHECK(wlan_ba_tx_waits(d, 0) == 0 && d->ba_tx[0].state == WLAN_BA_TX_REQUESTED);
    d->rekey_due = 0;
    CHECK(wlan_ba_tx_waits(d, 0) == now + WLAN_BA_TX_RESPONSE);
    wlan_ba_tx_stop(d, 0, 0);
    wlan_data_detach(&wlan);
}

int main(void)
{
    check_hold();
    check_eapol_room();
    check_upload_room();
    check_ack_room();
    check_stop_reasons();
    check_sleep_status();
    check_rx_sends();
    check_rx_full();
    check_rx_bound();
    check_status_full();
    check_other_refusal();
    check_stop();
    check_rekey_ring();
    check_rekey_hold();
    check_rekey_bound();
    check_rekey_eapol_full_ring();
    check_rekey_eapol_stopped();
    check_rekey_lost_first();
    check_rekey_lost();
    check_rekey_lost_delba();
    check_rekey_delba_hold();
    check_rekey_delba_first();
    check_ba_request_holds();
    check_ba_no_room();
    check_rekey_one_wait();
    check_rekey_gone();
    if (failures) {
        printf("wlan_txq_hosttest: %u failures\n", failures);
        return failures > 255 ? 255 : (int)failures;
    }
    printf("wlan_txq_hosttest: ok\n");
    return 0;
}
