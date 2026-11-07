/* Check the wireless transmit queue's back-pressure: a radio whose ring is
 * full holds the queue's frames until its completions free slots, in order
 * and with none dropped, the queue keeps its last slots for EAPOL, another
 * refusal and a stop still drop frames, and a pairwise key waits for what was
 * queued before it.
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

/** The kernel's tick, which the test leaves still. */
uint64_t tick_count(void)
{
    return now;
}

/** Queue work as work.c does, refusing a record already queued. */
int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    unsigned i;

    if (work->pending)
        return -EBUSY;
    for (i = 0; i < 8; ++i) {
        if (!queued[i]) {
            queued[i] = work;
            work->pending = 1;
            work->due = due;
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
        for (; i < 7; ++i)
            queued[i] = queued[i + 1];
        queued[7] = 0;
        w->pending = 0;
        w->run(w);
    }
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
static unsigned refuse_mark = 0xffff; /* a frame refused with -EIO */

/** Take a frame while the ring has room, as ath9k_tx does: -ENOBUFS when
 * it is full, and -EIO for the frame the test marks bad. */
static int radio_tx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                    const struct wlan_tx_info *info)
{
    unsigned at = MPDU_MARK + (frame[1] & 0x40 ? WLAN_CCMP_HDR_LEN : 0);
    unsigned mark = len > at ? frame[at] : 0xffff;

    (void)radio;
    if (mark == refuse_mark)
        return -EIO;
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
 * Eighteen frames into a ring of four behind the fourteen slots of the
 * queue that data may take: the ring fills, the queue holds the rest
 * without a drain queued per frame, the nineteenth is refused to its
 * sender, and completions one at a time send all eighteen in order with
 * consecutive sequence numbers.
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

/** A dead link does not hold the key past WLAN_DATA_REKEY_WAIT, and what
 * is left goes under the new one. */
static void check_rekey_bound(void)
{
    unsigned i;

    setup_keyed();
    for (i = 0; i < RING + 1; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    now += WLAN_DATA_REKEY_WAIT - 1;
    CHECK(pairwise(2) == -EINPROGRESS);
    now += 1;
    CHECK(pairwise(2) == 0);
    CHECK(wlan_data_of(&wlan)->pairwise[0].key.hw_slot == 1);
    complete(RING, 1);
    run_work();
    CHECK(n_taken == RING + 1 && marks[RING] == RING && slots[RING] == 1);
    wlan_data_detach(&wlan);
}

/** A connection lost during the wait ends the install with -ENOTCONN. */
static void check_rekey_lost(void)
{
    unsigned i;

    setup_keyed();
    for (i = 0; i < RING + 1; ++i)
        CHECK(send(i) == 0);
    run_work();
    CHECK(pairwise(2) == -EINPROGRESS);
    wlan_data_disconnect(wlan_data_of(&wlan));
    CHECK(pairwise(2) == -ENOTCONN);
    CHECK(!wlan_data_of(&wlan)->pairwise[0].gen);
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

int main(void)
{
    check_hold();
    check_eapol_room();
    check_other_refusal();
    check_stop();
    check_rekey_ring();
    check_rekey_hold();
    check_rekey_bound();
    check_rekey_lost();
    check_rekey_lost_delba();
    if (failures) {
        printf("wlan_txq_hosttest: %u failures\n", failures);
        return failures > 255 ? 255 : (int)failures;
    }
    printf("wlan_txq_hosttest: ok\n");
    return 0;
}
