/* Check the station's data path against a scripted AP and a stub radio:
 * transmit and receive under CCMP and TKIP in software and in a radio's key
 * slot, data refused without a key, replays, duplicates, fragments, BlockAck
 * reordering, a group rekey, BIP and protected management frames, A-MSDU and
 * rate selection.
 * host-deps: rach2/sys/network/wireless/tx.c
 * host-deps: rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/elems.c
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
#include "network/net.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "mm/kheap.h"
#include "lib/kirq.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            printf("wlan_data_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static const uint8_t sta[6] = {0x02, 0, 0, 0, 0, 0x01};
static const uint8_t bssid[6] = {0x02, 0, 0, 0, 0, 0xaa};
static const uint8_t far[6] = {0x02, 0, 0, 0, 0, 0xbb}; /* behind the AP */
static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t tk[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                               0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t gtk1[16] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                                 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x20};
static const uint8_t gtk2[16] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
                                 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x30};
static const uint8_t igtk[16] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                                 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x40};
/* TKIP: the temporal key, then the station's transmit and receive Michael
 * keys, which are the AP's receive and transmit keys. */
static const uint8_t tkip_key[32] = {
    0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x50,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78};
static const uint8_t ipv4_snap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00};
static const uint8_t eapol_snap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e};

/** The network lock, which data.c takes to count a dropped frame: nothing
 * to exclude in a test of one thread. */
uint32_t net_lock(void)
{
    return 0;
}

/** Release the network lock: nothing to restore. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** Compare n bytes: 0 when they are equal. */
static int cmp(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 1;
    return 0;
}

/* --- The stub radio. --- */

struct sent {
    uint8_t frame[IEEE80211_MAX_FRAME_LEN];
    unsigned len;
    struct wlan_tx_info info;
};

static struct sent sent[16];
static unsigned n_sent;
static int takes_keys;    /* set_key puts keys in slots */
static unsigned set_keys; /* installs into slots */
static unsigned del_keys; /* removals from slots */
static int slot_used[8];
static unsigned ampdu_starts, ampdu_stops;
static int radio_full; /* radio_tx refuses each frame */
static unsigned rate_selects, rate_statuses;

/** Record a transmitted frame. */
static int radio_tx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                    const struct wlan_tx_info *info)
{
    struct sent *s = &sent[n_sent % 16];

    (void)radio;
    if (radio_full)
        return -ENOBUFS;
    k_memcpy(s->frame, frame, len);
    s->len = len;
    s->info = *info;
    ++n_sent;
    return 0;
}

/** Take a key into a free slot when takes_keys is set; refuse otherwise. */
static int radio_set_key(struct wlan_radio *radio, int enable, const uint8_t peer[6],
                         const struct wlan_key *key, int *slot)
{
    int i;

    (void)radio;
    (void)peer;
    (void)key;
    if (!enable) {
        CHECK(*slot >= 0 && *slot < 8 && slot_used[*slot]);
        slot_used[*slot] = 0;
        ++del_keys;
        return 0;
    }
    if (!takes_keys)
        return -EOPNOTSUPP;
    for (i = 0; i < 8; ++i) {
        if (!slot_used[i]) {
            slot_used[i] = 1;
            *slot = i;
            ++set_keys;
            return 0;
        }
    }
    return -ENOSPC;
}

/** Count BlockAck sessions. */
static int radio_ampdu(struct wlan_radio *radio, enum wlan_ampdu_action action,
                       const uint8_t peer[6], unsigned tid, unsigned ssn, unsigned window)
{
    (void)radio;
    (void)peer;
    (void)tid;
    (void)ssn;
    (void)window;
    if (action == WLAN_AMPDU_RX_START)
        ++ampdu_starts;
    if (action == WLAN_AMPDU_RX_STOP)
        ++ampdu_stops;
    return 0;
}

/** Start, stop and configure do nothing in the stub. */
static int radio_start(struct wlan_radio *radio)
{
    (void)radio;
    return 0;
}

/** Nothing to stop. */
static void radio_stop(struct wlan_radio *radio)
{
    (void)radio;
}

/** Nothing to tune. */
static int radio_config(struct wlan_radio *radio, const struct wlan_channel *channel,
                        unsigned filters)
{
    (void)radio;
    (void)channel;
    (void)filters;
    return 0;
}

static const struct wlan_radio_ops ops = {
    radio_start, radio_stop, radio_config, radio_tx, radio_set_key, 0, radio_ampdu, 0, 0, 0};

/** A rate algorithm that picks the fastest rate once. */
static void rc_select(struct wlan_radio *radio, const struct wlan_bss *peer, const uint8_t *frame,
                      unsigned len, struct wlan_tx_rate *rates)
{
    unsigned i;

    (void)peer;
    (void)frame;
    (void)len;
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        rates[i].index = -1;
        rates[i].count = 0;
        rates[i].flags = 0;
    }
    rates[0].index = (int16_t)(radio->band.n_rates - 1);
    rates[0].count = 1;
    ++rate_selects;
}

/** Count the statuses rate control learns. */
static void rc_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    (void)radio;
    (void)status;
    ++rate_statuses;
}

static const struct wlan_rate_ops rc_ops = {rc_select, rc_status};

/* --- The worker, the clock and the callbacks. --- */

static uint64_t now;
static struct wlan_work *queued[8];

/** The test runs on one thread with no interrupts to mask. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing was masked. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** The kernel's tick, which the test moves. */
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

/** Run the work due by now. */
static void run_work(void)
{
    struct wlan_work *w;
    unsigned i;

    for (i = 0; i < 8; ++i) {
        w = queued[i];
        if (w && w->due <= now) {
            queued[i] = 0;
            w->pending = 0;
            w->run(w);
        }
    }
}

static uint8_t got[8][IEEE80211_MAX_FRAME_LEN];
static unsigned got_len[8];
static unsigned n_got;

/** Record a delivered MSDU. */
static int on_deliver(struct wlan_data *d, const uint8_t *frame, unsigned len)
{
    (void)d;
    CHECK(len <= IEEE80211_MAX_FRAME_LEN);
    k_memcpy(got[n_got % 8], frame, len);
    got_len[n_got % 8] = len;
    ++n_got;
    return 0;
}

static struct wlan_event events[8];
static unsigned n_events;

/** Record an event. */
static void on_event(struct wlan *wlan, const struct wlan_event *event)
{
    (void)wlan;
    events[n_events % 8] = *event;
    ++n_events;
}

/* --- The station under test. --- */

static struct wlan_radio radio;
static struct wlan_counters counters;
static struct wlan_data *d;
static struct wlan_bss bss;
static struct wlan_aes128 ap_ptk, ap_gtk1, ap_gtk2, ap_igtk;
static const struct wlan_rx_status rxs = {0, 2412, -40, 0, 0, 0, 0, 0, -1};

/** Make the radio, the BSS and a connected data path, with no keys. */
static void setup(int qos)
{
    static const uint16_t rates[12] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};
    static const uint8_t ies[] = {0,    4,    'w',  'i',  'f',  'i',  1,    8,  0x82,
                                  0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4,
                                  0x30, 0x48, 0x60, 0x6c, 3,    1,    1};
    unsigned i;

    k_memset(&radio, 0, sizeof(radio));
    radio.ops = &ops;
    radio.name = "stub";
    k_memcpy(radio.mac, sta, 6);
    for (i = 0; i < 12; ++i)
        radio.band.rates[i] = rates[i];
    radio.band.n_rates = 12;
    radio.band.n_channels = 1;
    radio.band.channels[0].frequency = 2412;
    radio.band.channels[0].number = 1;
    radio.hw_ciphers = WLAN_CIPHER_HW_CCMP | WLAN_CIPHER_HW_TKIP;
    k_memset(&bss, 0, sizeof(bss));
    k_memcpy(bss.bssid, bssid, 6);
    bss.frequency = 2412;
    bss.capability = WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY;
    k_memcpy(bss.ies, ies, sizeof(ies));
    bss.ies_len = sizeof(ies);
    k_memset(&counters, 0, sizeof(counters));
    if (!d)
        d = kzalloc(sizeof(*d));
    wlan_data_init(d, 0, &radio, &counters, on_deliver, on_event);
    CHECK(wlan_data_connect(d, &bss, qos, 1, 0) == 0);
    n_sent = n_got = n_events = 0;
    takes_keys = 0;
    set_keys = del_keys = 0;
    k_memset(slot_used, 0, sizeof(slot_used));
    for (i = 0; i < 8; ++i)
        queued[i] = 0;
    wlan_aes128_setkey(&ap_ptk, tk);
    wlan_aes128_setkey(&ap_gtk1, gtk1);
    wlan_aes128_setkey(&ap_gtk2, gtk2);
    wlan_aes128_setkey(&ap_igtk, igtk);
}

/** Install a key of `cipher` at `index`, pairwise or group, with every
 * receive counter at `rsc`. */
static int install(uint32_t cipher, unsigned index, int pairwise, const uint8_t *bytes,
                   unsigned len, uint64_t rsc)
{
    struct wlan_key k;
    unsigned i;

    k_memset(&k, 0, sizeof(k));
    k.cipher = cipher;
    k.index = index;
    k.flags = pairwise ? WLAN_KEY_PAIRWISE : 0;
    k_memcpy(k.peer, pairwise ? bssid : bcast, 6);
    k_memcpy(k.data, bytes, len);
    k.len = len;
    k.hw_slot = -1;
    for (i = 0; i < WLAN_NUM_TIDS; ++i)
        k.rx_pn[i] = rsc;
    return wlan_data_install_key(d, &k);
}

/** Build a FromDS data frame to `da` with an LLC/SNAP payload `body` of n
 * bytes; return its length. */
static unsigned ap_frame(uint8_t *f, const uint8_t *da, int qos, unsigned tid, unsigned seq,
                         unsigned frag, int more, const uint8_t *body, unsigned n)
{
    unsigned h = qos ? 26 : 24;

    k_memset(f, 0, h);
    f[0] = (uint8_t)(IEEE80211_FTYPE_DATA | (qos ? IEEE80211_STYPE_QOS_DATA : 0));
    f[1] = (uint8_t)((IEEE80211_FCTL_FROMDS | (more ? IEEE80211_FCTL_MOREFRAGS : 0)) >> 8);
    k_memcpy(f + 4, da, 6);
    k_memcpy(f + 10, bssid, 6);
    k_memcpy(f + 16, far, 6);
    f[22] = (uint8_t)(seq << 4 | frag);
    f[23] = (uint8_t)(seq >> 4);
    if (qos)
        f[24] = (uint8_t)tid;
    k_memcpy(f + h, body, n);
    return h + n;
}

/** A payload: an LLC/SNAP header of IPv4 and `n - 8` bytes counting from
 * `seed`. */
static void payload(uint8_t *p, unsigned n, unsigned seed)
{
    unsigned i;

    k_memcpy(p, ipv4_snap, 8);
    for (i = 8; i < n; ++i)
        p[i] = (uint8_t)(seed + i);
}

/** Encrypt as the AP does under CCMP. */
static void ap_ccmp(const struct wlan_aes128 *key, uint8_t *f, unsigned *len, uint64_t pn,
                    unsigned keyid)
{
    CHECK(wlan_ccmp_encrypt(key, f, len, IEEE80211_MAX_FRAME_LEN, pn, keyid) == 0);
}

/** Encrypt an MPDU as the AP does under TKIP: the IV, then RC4 over the
 * body and its ICV. The caller appended any Michael MIC. */
static void ap_tkip(uint8_t *f, unsigned *len, unsigned hdrlen, uint64_t tsc, unsigned keyid)
{
    uint16_t ttak[5];
    uint8_t rc4key[16];
    unsigned body = *len - hdrlen;
    uint32_t icv;

    k_memmove(f + hdrlen + 8, f + hdrlen, body);
    wlan_tkip_mix1(tkip_key, bssid, (uint32_t)(tsc >> 16), ttak);
    wlan_tkip_mix2(tkip_key, ttak, (uint16_t)tsc, rc4key);
    f[hdrlen] = rc4key[0];
    f[hdrlen + 1] = rc4key[1];
    f[hdrlen + 2] = rc4key[2];
    f[hdrlen + 3] = (uint8_t)(0x20 | keyid << 6);
    f[hdrlen + 4] = (uint8_t)(tsc >> 16);
    f[hdrlen + 5] = (uint8_t)(tsc >> 24);
    f[hdrlen + 6] = (uint8_t)(tsc >> 32);
    f[hdrlen + 7] = (uint8_t)(tsc >> 40);
    icv = wlan_crc32(f + hdrlen + 8, body);
    f[hdrlen + 8 + body] = (uint8_t)icv;
    f[hdrlen + 9 + body] = (uint8_t)(icv >> 8);
    f[hdrlen + 10 + body] = (uint8_t)(icv >> 16);
    f[hdrlen + 11 + body] = (uint8_t)(icv >> 24);
    wlan_tkip_rc4(rc4key, f + hdrlen + 8, body + 4);
    f[1] |= IEEE80211_FCTL_PROTECTED >> 8;
    *len += 12;
}

/** Receive a frame of the AP's with the plain status. */
static int rx(uint8_t *f, unsigned len)
{
    return wlan_data_rx(d, f, len, &rxs);
}

/** Build a station data frame, the form wlan_encap makes, to `far`. */
static unsigned sta_frame(uint8_t *f, const uint8_t *snap, unsigned n)
{
    unsigned i;

    k_memset(f, 0, 24);
    f[0] = IEEE80211_FTYPE_DATA;
    f[1] = IEEE80211_FCTL_TODS >> 8;
    k_memcpy(f + 4, bssid, 6);
    k_memcpy(f + 10, sta, 6);
    k_memcpy(f + 16, far, 6);
    k_memcpy(f + 24, snap, 8);
    for (i = 32; i < 24 + n; ++i)
        f[i] = (uint8_t)i;
    return 24 + n;
}

/* --- The checks. --- */

/** The key table's refusals, and keys kept or replaced. */
static void check_keys(void)
{
    struct wlan_key k;
    struct wlan_data_key *p;

    setup(1);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tkip_key, 32, 0) == -EINVAL);
    CHECK(install(WLAN_CIPHER_SUITE_AES_CMAC, 4, 1, igtk, 16, 0) == -EINVAL);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 4, 0, gtk1, 16, 0) == -EINVAL);
    CHECK(install(WLAN_CIPHER_SUITE_AES_CMAC, 3, 0, igtk, 16, 0) == -EINVAL);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 2, 1, tk, 16, 0) == -EINVAL);
    CHECK(install(0x000fac01u, 1, 0, gtk1, 5, 0) == -EINVAL); /* WEP */
    k_memset(&k, 0, sizeof(k));
    k.cipher = WLAN_CIPHER_SUITE_CCMP;
    k.flags = WLAN_KEY_PAIRWISE;
    k_memcpy(k.peer, bcast, 6);
    k.len = 16;
    CHECK(wlan_data_install_key(d, &k) == -EINVAL);
    CHECK(wlan_data_remove_key(d, bssid, 0) == -ENOENT);
    CHECK(wlan_data_default_key(d, 1, 0) == -ENOENT);
    CHECK(wlan_data_default_key(d, 1, 1) == -EINVAL);

    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    p = wlan_data_pairwise_key(d, 0);
    CHECK(p && p->key.hw_slot == -1 && p->gen);
    p->key.tx_pn = 77;
    p->key.rx_pn[3] = 55;
    /* The same key again, as a replayed message 3 installs it, keeps its
     * counters. */
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(p->key.tx_pn == 77 && p->key.rx_pn[3] == 55);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk2, 16, 0) == 0);
    CHECK(p->key.tx_pn == 0 && p->key.rx_pn[3] == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 1, 0, gtk1, 16, 9) == 0);
    CHECK(wlan_data_default_key(d, 1, 0) == 0 && d->default_group == 1);
    CHECK(install(WLAN_CIPHER_SUITE_AES_CMAC, 4, 0, igtk, 16, 0) == 0);
    CHECK(wlan_data_default_key(d, 4, 1) == 0 && d->default_igtk == 4);
    CHECK(wlan_data_group_key(d, 1)->key.rx_pn[0] == 9);
    CHECK(wlan_data_remove_key(d, bcast, 1) == 0 && !wlan_data_group_key(d, 1));
    CHECK(wlan_data_remove_key(d, far, 0) == -ENOENT);
    CHECK(wlan_data_remove_key(d, bssid, 0) == 0 && !wlan_data_pairwise_key(d, 0));
}

/** Transmit in clear on an open network, then under CCMP in software. */
static void check_tx_ccmp(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN], plain[200];
    unsigned len, n, keyid;
    uint64_t pn;

    setup(1);
    CHECK(wlan_data_connect(d, &bss, 1, 0, 0) == 0);
    len = sta_frame(f, ipv4_snap, 100);
    k_memcpy(plain, f, len);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 5) == 0);
    CHECK(n_sent == 1);
    CHECK(sent[0].len == len + 2);
    CHECK(sent[0].frame[0] == (IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA));
    CHECK(sent[0].frame[24] == 5 && sent[0].frame[22] == 0 && sent[0].frame[23] == 0);
    CHECK(sent[0].info.tid == 5 && sent[0].info.ac == WLAN_AC_VI);
    CHECK(sent[0].info.cookie & WLAN_DATA_COOKIE);
    CHECK(sent[0].info.frequency == 2412 && sent[0].info.hw_key == -1);
    /* The basic table: 11, 5.5, 2 and 1 Mbit/s, fastest first. */
    CHECK(sent[0].info.rates[0].index == 3 && sent[0].info.rates[0].count == 2);
    CHECK(sent[0].info.rates[1].index == 2 && sent[0].info.rates[2].index == 1);
    CHECK(sent[0].info.rates[3].index == 0 && sent[0].info.rates[3].count == 4);
    CHECK(counters.tx_packets == 1);
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 5) == 0);
    CHECK(sent[1].frame[22] == 0x10); /* TID 5's second */
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == 0);
    CHECK(sent[2].frame[22] == 0 && sent[2].info.ac == WLAN_AC_BE); /* TID 0's first */
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 1) == 0);
    CHECK(sent[3].info.ac == WLAN_AC_BK);
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 7) == 0);
    CHECK(sent[4].info.ac == WLAN_AC_VO);

    /* EAPOL goes at the lowest basic rate alone. */
    len = sta_frame(f, eapol_snap, 60);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 7) == 0);
    CHECK(sent[5].info.rates[0].index == 0 && sent[5].info.rates[1].index == -1);

    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == 0);
    n = sent[6].len;
    CHECK(n == len + 2 + 16 && (sent[6].frame[1] & (IEEE80211_FCTL_PROTECTED >> 8)));
    CHECK(wlan_ccmp_decrypt(&ap_ptk, sent[6].frame, &n, &pn, &keyid) == 0);
    CHECK(pn == 1 && keyid == 0 && n == len + 2);
    CHECK(!cmp(sent[6].frame + 26, plain + 24, len - 24));
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == 0);
    n = sent[7].len;
    CHECK(wlan_ccmp_decrypt(&ap_ptk, sent[7].frame, &n, &pn, &keyid) == 0 && pn == 2);
    /* Room is checked before anything changes. */
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, len + 10, 0) == -ENOSPC);
    /* A spent PN refuses rather than wrap. */
    wlan_data_pairwise_key(d, 0)->key.tx_pn = WLAN_PN_MAX;
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == -EOVERFLOW);
    /* Not data from the station. */
    f[1] = IEEE80211_FCTL_FROMDS >> 8;
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == -EINVAL);
}

/** A protected network sends EAPOL alone in clear: before the pairwise
 * key, and after its removal, which must not leave data unprotected. */
static void check_tx_nokey(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN];
    unsigned len;

    setup(1);
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == -ENOKEY && n_sent == 0);
    len = sta_frame(f, eapol_snap, 60);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 7) == 0 && n_sent == 1);
    CHECK(!(sent[0].frame[1] & (IEEE80211_FCTL_PROTECTED >> 8)));
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == 0 && n_sent == 2);
    CHECK(sent[1].frame[1] & (IEEE80211_FCTL_PROTECTED >> 8));
    /* NL80211_CMD_DEL_KEY on the authorized connection. */
    CHECK(wlan_data_remove_key(d, bssid, 0) == 0);
    len = sta_frame(f, ipv4_snap, 100);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == -ENOKEY && n_sent == 2);
    len = sta_frame(f, eapol_snap, 60);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 7) == 0 && n_sent == 3);
    CHECK(!(sent[2].frame[1] & (IEEE80211_FCTL_PROTECTED >> 8)));
}

/** Transmit under a key the radio holds: headers and room, its slot. */
static void check_tx_hw(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN];
    unsigned len;
    struct wlan_data_key *p;

    setup(0);
    takes_keys = 1;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    p = wlan_data_pairwise_key(d, 0);
    CHECK(p->key.hw_slot == 0 && set_keys == 1);
    len = sta_frame(f, ipv4_snap, 64);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == 0);
    CHECK(sent[0].info.flags & WLAN_TX_HW_KEY && sent[0].info.hw_key == 0);
    CHECK(sent[0].info.tid == WLAN_NON_QOS_TID);
    CHECK(sent[0].len == len + 16 && (sent[0].frame[1] & (IEEE80211_FCTL_PROTECTED >> 8)));
    /* The CCMP header carries the data path's PN 1, key 0, Ext IV. */
    CHECK(sent[0].frame[24] == 1 && sent[0].frame[25] == 0 && sent[0].frame[27] == 0x20);
    CHECK(sent[0].frame[32] == 0xaa && sent[0].frame[len + 8] == 0);
    /* A restarted radio takes its keys again; removal empties the slot. */
    slot_used[0] = 0;
    wlan_data_restore_keys(d);
    CHECK(set_keys == 2 && p->key.hw_slot == 0);
    /* A new key replaces the old in the radio. */
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0);
    CHECK(set_keys == 3 && del_keys == 1 && p->key.hw_slot == 1 && !slot_used[0]);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 2, 0, gtk2, 16, 0) == 0);
    wlan_data_disconnect(d);
    CHECK(del_keys == 3 && !slot_used[0] && !slot_used[1]);
    CHECK(!wlan_data_pairwise_key(d, 0) && !wlan_data_group_key(d, 2));
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == -ENOTCONN);
}

/** Receive under CCMP: in software, replays per TID, a forged tag, by the
 * radio, duplicates and unprotected data. */
static void check_rx_ccmp(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN], g[IEEE80211_MAX_FRAME_LEN], body[120];
    struct wlan_rx_status hw = rxs;
    unsigned len, glen;

    setup(1);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    payload(body, sizeof(body), 1);
    len = ap_frame(f, sta, 1, 2, 10, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 10, 0);
    k_memcpy(g, f, len);
    glen = len;
    CHECK(rx(f, len) == 0 && n_got == 1);
    CHECK(got_len[0] == 26 + sizeof(body) && !(got[0][1] & (IEEE80211_FCTL_PROTECTED >> 8)));
    CHECK(!cmp(got[0] + 26, body, sizeof(body)));
    CHECK(counters.rx_packets == 1 && counters.signal == -4000 && counters.quality == 70);
    /* The same PN again, now as a new frame, is a replay. */
    g[22] = 0xb0;
    CHECK(rx(g, glen) == -ESTALE && counters.rx_replays == 1 && n_got == 1);
    len = ap_frame(f, sta, 1, 2, 12, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 9, 0);
    CHECK(rx(f, len) == -ESTALE && counters.rx_replays == 2);
    /* Each TID keeps its own counter. */
    len = ap_frame(f, sta, 1, 3, 13, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 5, 0);
    CHECK(rx(f, len) == 0 && n_got == 2);
    /* A changed byte fails the tag. */
    len = ap_frame(f, sta, 1, 2, 14, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 11, 0);
    f[40] ^= 1;
    CHECK(rx(f, len) == -EBADMSG && counters.rx_decrypt_errors == 1);
    /* A key id with no key. */
    len = ap_frame(f, sta, 1, 2, 15, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 12, 1);
    CHECK(rx(f, len) == -ENOKEY);
    /* A retry of a frame received is a duplicate before any decryption. */
    len = ap_frame(f, sta, 1, 2, 16, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 13, 0);
    k_memcpy(g, f, len);
    CHECK(rx(f, len) == 0 && n_got == 3);
    g[1] |= IEEE80211_FCTL_RETRY >> 8;
    CHECK(rx(g, len) == -EALREADY && counters.rx_duplicates == 1 && n_got == 3);

    /* The radio decrypted and stripped it: its PN still meets the replay
     * check. */
    hw.flags = WLAN_RX_DECRYPTED | WLAN_RX_IV_STRIPPED | WLAN_RX_MIC_STRIPPED;
    hw.pn = 20;
    hw.key_index = 0;
    len = ap_frame(f, sta, 1, 2, 20, 0, 0, body, sizeof(body));
    CHECK(wlan_data_rx(d, f, len, &hw) == 0 && n_got == 4 && got_len[3] == len);
    len = ap_frame(f, sta, 1, 2, 21, 0, 0, body, sizeof(body));
    CHECK(wlan_data_rx(d, f, len, &hw) == -ESTALE && n_got == 4);
    /* Decrypted with the header and MIC left in place. */
    len = ap_frame(f, sta, 1, 2, 22, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 21, 0);
    hw.flags = WLAN_RX_DECRYPTED;
    k_memcpy(f + 34, body, sizeof(body)); /* the plaintext the radio left */
    CHECK(wlan_data_rx(d, f, len, &hw) == 0 && n_got == 5);
    CHECK(got_len[4] == 26 + sizeof(body) && !cmp(got[4] + 26, body, sizeof(body)));
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[2] == 21);

    /* In clear on a protected network only EAPOL, and only whole. */
    len = ap_frame(f, sta, 1, 2, 30, 0, 0, body, sizeof(body));
    CHECK(rx(f, len) == -EACCES && n_got == 5);
    k_memcpy(body, eapol_snap, 8);
    len = ap_frame(f, sta, 1, 2, 31, 0, 0, body, sizeof(body));
    CHECK(rx(f, len) == 0 && n_got == 6);
    len = ap_frame(f, sta, 1, 2, 32, 0, 1, body, sizeof(body));
    CHECK(rx(f, len) == -EACCES);
    /* Another BSS, another station, our own frame come back, a null. */
    len = ap_frame(f, far, 1, 2, 33, 0, 0, body, sizeof(body));
    CHECK(rx(f, len) == -EINVAL);
    len = ap_frame(f, bcast, 0, 0, 34, 0, 0, body, sizeof(body));
    k_memcpy(f + 16, sta, 6);
    CHECK(rx(f, len) == -EINVAL);
    len = ap_frame(f, sta, 0, 0, 35, 0, 0, body, 0);
    f[0] |= IEEE80211_STYPE_NULLFUNC;
    CHECK(rx(f, len) == 0 && n_got == 6);
    len = ap_frame(f, sta, 1, 2, 36, 0, 0, body, sizeof(body));
    f[10] ^= 1;
    CHECK(rx(f, len) == -EINVAL);
}

/** Reassembly under CCMP: whole, out of order, a PN gap, a key change, a
 * plaintext fragment and a timeout. */
static void check_fragments(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN], body[300];
    unsigned len, i;

    setup(1);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    payload(body, sizeof(body), 7);
    for (i = 0; i < 3; ++i) {
        len = ap_frame(f, sta, 1, 0, 40, i, i < 2, body + i * 100, 100);
        ap_ccmp(&ap_ptk, f, &len, 100 + i, 0);
        CHECK(rx(f, len) == 0);
        CHECK(n_got == (i == 2 ? 1u : 0u));
    }
    CHECK(got_len[0] == 26 + sizeof(body) && !cmp(got[0] + 26, body, sizeof(body)));
    CHECK(!(got[0][1] & (IEEE80211_FCTL_MOREFRAGS >> 8)) && (got[0][22] & 0xf) == 0);
    CHECK(counters.rx_fragments == 3);

    /* Fragment 2 without fragment 1. */
    len = ap_frame(f, sta, 1, 0, 41, 0, 1, body, 100);
    ap_ccmp(&ap_ptk, f, &len, 110, 0);
    CHECK(rx(f, len) == 0);
    len = ap_frame(f, sta, 1, 0, 41, 2, 0, body + 200, 100);
    ap_ccmp(&ap_ptk, f, &len, 112, 0);
    CHECK(rx(f, len) == -EINVAL && n_got == 1);
    /* Consecutive fragments whose PNs are not. */
    len = ap_frame(f, sta, 1, 0, 42, 0, 1, body, 100);
    ap_ccmp(&ap_ptk, f, &len, 120, 0);
    CHECK(rx(f, len) == 0);
    len = ap_frame(f, sta, 1, 0, 42, 1, 0, body + 100, 100);
    ap_ccmp(&ap_ptk, f, &len, 122, 0);
    CHECK(rx(f, len) == -EINVAL && n_got == 1);
    /* A new key drops what the old one began. */
    len = ap_frame(f, sta, 1, 0, 43, 0, 1, body, 100);
    ap_ccmp(&ap_ptk, f, &len, 130, 0);
    CHECK(rx(f, len) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 1, 1, gtk2, 16, 0) == 0);
    len = ap_frame(f, sta, 1, 0, 43, 1, 0, body + 100, 100);
    ap_ccmp(&ap_ptk, f, &len, 131, 0);
    CHECK(rx(f, len) == -EINVAL && n_got == 1);
    /* Too slow. */
    len = ap_frame(f, sta, 1, 0, 44, 0, 1, body, 100);
    ap_ccmp(&ap_ptk, f, &len, 140, 0);
    CHECK(rx(f, len) == 0);
    now += WLAN_DATA_FRAG_TIMEOUT;
    len = ap_frame(f, sta, 1, 0, 44, 1, 0, body + 100, 100);
    ap_ccmp(&ap_ptk, f, &len, 141, 0);
    CHECK(rx(f, len) == -EINVAL && n_got == 1);
    /* Group-addressed fragments are refused. */
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 1, 0, gtk1, 16, 0) == 0);
    len = ap_frame(f, bcast, 0, 0, 45, 0, 1, body, 100);
    ap_ccmp(&ap_gtk1, f, &len, 1, 1);
    CHECK(rx(f, len) == -EINVAL);
}

/** TKIP both ways: Michael, the ICV, a forged MIC, a replay, fragments. */
static void check_tkip(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN], msdu[200], mic[8], rc4key[16];
    uint16_t ttak[5];
    unsigned len, n, h = 26;
    uint64_t tsc;
    uint32_t icv;

    setup(1);
    CHECK(install(WLAN_CIPHER_SUITE_TKIP, 0, 1, tkip_key, 32, 0) == 0);
    payload(msdu, 120, 3);
    /* The AP's MIC over DA, SA, the priority and the MSDU. */
    wlan_tkip_michael(tkip_key + 24, sta, far, 4, msdu, 120, mic);
    k_memcpy(msdu + 120, mic, 8);
    len = ap_frame(f, sta, 1, 4, 50, 0, 0, msdu, 128);
    ap_tkip(f, &len, h, 0x10005, 0);
    CHECK(rx(f, len) == 0 && n_got == 1);
    CHECK(got_len[0] == h + 120 && !cmp(got[0] + h, msdu, 120));
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[4] == 0x10005);
    /* The same TSC again. */
    len = ap_frame(f, sta, 1, 4, 51, 0, 0, msdu, 128);
    ap_tkip(f, &len, h, 0x10005, 0);
    CHECK(rx(f, len) == -ESTALE && counters.rx_replays == 1);
    /* A wrong ICV is a decryption error and no MIC failure. */
    len = ap_frame(f, sta, 1, 4, 52, 0, 0, msdu, 128);
    ap_tkip(f, &len, h, 0x10006, 0);
    f[len - 1] ^= 0x80;
    CHECK(rx(f, len) == -EBADMSG && counters.rx_decrypt_errors == 1 && n_events == 0);
    /* A wrong MIC under a right ICV is reported for the countermeasures,
     * and its TSC is not taken. */
    msdu[125] ^= 1;
    len = ap_frame(f, sta, 1, 4, 53, 0, 0, msdu, 128);
    ap_tkip(f, &len, h, 0x10007, 0);
    CHECK(rx(f, len) == -EBADMSG && counters.rx_mic_errors == 1);
    CHECK(n_events == 1 && events[0].type == WLAN_EVENT_MIC_FAILURE);
    CHECK(events[0].pairwise == 1 && events[0].key_index == 0);
    CHECK(events[0].peer[5] == 0xaa);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[4] == 0x10005);
    msdu[125] ^= 1;
    /* The MSDU in two fragments, its MIC split between them. */
    len = ap_frame(f, sta, 1, 4, 54, 0, 1, msdu, 124);
    ap_tkip(f, &len, h, 0x10008, 0);
    CHECK(rx(f, len) == 0 && n_got == 1);
    len = ap_frame(f, sta, 1, 4, 54, 1, 0, msdu + 124, 4);
    ap_tkip(f, &len, h, 0x10009, 0);
    CHECK(rx(f, len) == 0 && n_got == 2);
    CHECK(got_len[1] == h + 120 && !cmp(got[1] + h, msdu, 120));
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[4] == 0x10009);
    /* Fragments whose headers differ in length, 26 bytes and then 30 with
     * HT Control, both bodies empty: the MSDU would be shorter than the last
     * header, and Michael would read past it. */
    len = ap_frame(f, sta, 1, 4, 56, 0, 1, msdu, 0);
    ap_tkip(f, &len, h, 0x1000a, 0);
    CHECK(rx(f, len) == 0);
    len = ap_frame(f, sta, 1, 4, 56, 1, 0, msdu, 0);
    f[1] |= IEEE80211_FCTL_ORDER >> 8;
    k_memset(f + h, 0, IEEE80211_HT_CTL_LEN);
    len += IEEE80211_HT_CTL_LEN;
    ap_tkip(f, &len, h + IEEE80211_HT_CTL_LEN, 0x1000b, 0);
    n = counters.rx_dropped;
    CHECK(rx(f, len) == -EINVAL && counters.rx_dropped == n + 1);
    CHECK(n_got == 2 && n_events == 1);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[4] == 0x10009);
    /* The radio found the MIC wrong. */
    {
        struct wlan_rx_status hw = rxs;

        hw.flags =
            WLAN_RX_DECRYPTED | WLAN_RX_IV_STRIPPED | WLAN_RX_MIC_STRIPPED | WLAN_RX_MIC_ERROR;
        hw.pn = 0x20000;
        len = ap_frame(f, sta, 1, 4, 55, 0, 0, msdu, 120);
        CHECK(wlan_data_rx(d, f, len, &hw) == -EBADMSG && n_events == 2);
    }

    /* The station's own: the AP checks the ICV and its Michael key. */
    len = sta_frame(f, ipv4_snap, 80);
    k_memcpy(msdu, f + 24, 80);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 6) == 0);
    n = sent[0].len;
    CHECK(n == len + 2 + 8 + 8 + 4);
    tsc = (uint64_t)sent[0].frame[h + 2] | (uint64_t)sent[0].frame[h] << 8 |
          (uint64_t)sent[0].frame[h + 4] << 16;
    CHECK(tsc == 1 && sent[0].frame[h + 3] == 0x20);
    wlan_tkip_mix1(tkip_key, sta, 0, ttak);
    wlan_tkip_mix2(tkip_key, ttak, 1, rc4key);
    wlan_tkip_rc4(rc4key, sent[0].frame + h + 8, n - h - 8);
    icv = wlan_crc32(sent[0].frame + h + 8, 80 + 8);
    CHECK(sent[0].frame[n - 4] == (uint8_t)icv && sent[0].frame[n - 1] == (uint8_t)(icv >> 24));
    wlan_tkip_michael(tkip_key + 16, far, sta, 6, msdu, 80, mic);
    CHECK(!cmp(sent[0].frame + h + 8, msdu, 80));
    CHECK(!cmp(sent[0].frame + h + 8 + 80, mic, 8));
}

/** A group rekey keeps the pairwise key and each key's counters. */
static void check_rekey(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN], body[60];
    unsigned len;

    setup(0);
    payload(body, sizeof(body), 9);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 1, 0, gtk1, 16, 4) == 0);
    /* At or below the RSC the handshake gave is a replay. */
    len = ap_frame(f, bcast, 0, 0, 60, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_gtk1, f, &len, 4, 1);
    CHECK(rx(f, len) == -ESTALE);
    len = ap_frame(f, bcast, 0, 0, 61, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_gtk1, f, &len, 5, 1);
    CHECK(rx(f, len) == 0 && n_got == 1);
    /* The rekey brings key 2; key 1 stays for frames still in flight. */
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 2, 0, gtk2, 16, 0) == 0);
    len = ap_frame(f, bcast, 0, 0, 62, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_gtk2, f, &len, 1, 2);
    CHECK(rx(f, len) == 0 && n_got == 2);
    len = ap_frame(f, bcast, 0, 0, 63, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_gtk1, f, &len, 6, 1);
    CHECK(rx(f, len) == 0 && n_got == 3);
    len = ap_frame(f, sta, 0, 0, 64, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_ptk, f, &len, 1, 0);
    CHECK(rx(f, len) == 0 && n_got == 4);
    /* Key 1 again with the same bytes keeps its counter, so the AP's old
     * frame stays a replay; a new key 1 starts afresh. */
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 1, 0, gtk1, 16, 0) == 0);
    len = ap_frame(f, bcast, 0, 0, 65, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_gtk1, f, &len, 6, 1);
    CHECK(rx(f, len) == -ESTALE);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 1, 0, gtk2, 16, 0) == 0);
    len = ap_frame(f, bcast, 0, 0, 66, 0, 0, body, sizeof(body));
    ap_ccmp(&ap_gtk2, f, &len, 1, 1);
    CHECK(rx(f, len) == 0 && n_got == 5);
    CHECK(wlan_data_pairwise_key(d, 0) != 0);
}

/** Build the AP's ADDBA request for TID `tid`. */
static unsigned addba_req(uint8_t *f, unsigned tid, unsigned window, unsigned ssn)
{
    unsigned params = 0x0002 | tid << 2 | window << 6;

    k_memset(f, 0, 33);
    f[0] = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION;
    k_memcpy(f + 4, sta, 6);
    k_memcpy(f + 10, bssid, 6);
    k_memcpy(f + 16, bssid, 6);
    f[24] = WLAN_CATEGORY_BACK;
    f[25] = WLAN_ACTION_ADDBA_REQ;
    f[26] = 7;
    f[27] = (uint8_t)params;
    f[28] = (uint8_t)(params >> 8);
    f[29] = 0;
    f[30] = 0;
    f[31] = (uint8_t)(ssn << 4);
    f[32] = (uint8_t)(ssn >> 4);
    return 33;
}

/** Send QoS data of TID 0 with sequence number sn, in clear. */
static int rx_sn(unsigned sn)
{
    uint8_t f[200], body[40];
    unsigned len;

    payload(body, sizeof(body), sn);
    len = ap_frame(f, sta, 1, 0, sn, 0, 0, body, sizeof(body));
    return rx(f, len);
}

/** The sequence number of the i-th MSDU delivered. */
static unsigned got_sn(unsigned i)
{
    return ((unsigned)got[i % 8][22] | (unsigned)got[i % 8][23] << 8) >> 4;
}

/** A frame of `tid` the radio opened, stripped, under key slot `slot` with
 * `pn`, heard on `frequency` MHz. */
static int rx_opened_on(unsigned frequency, unsigned tid, unsigned sn, int slot, uint64_t pn)
{
    uint8_t f[200], body[40];
    struct wlan_rx_status hw = rxs;
    unsigned len;

    hw.frequency = (uint16_t)frequency;
    hw.flags = WLAN_RX_DECRYPTED | WLAN_RX_IV_STRIPPED | WLAN_RX_MIC_STRIPPED;
    hw.hw_key = slot;
    hw.pn = pn;
    payload(body, sizeof(body), sn);
    len = ap_frame(f, sta, 1, tid, sn, 0, 0, body, sizeof(body));
    return wlan_data_rx(d, f, len, &hw);
}

/** A frame of `tid` the radio opened on the connection's channel; as
 * rx_opened_on. */
static int rx_opened_tid(unsigned tid, unsigned sn, int slot, uint64_t pn)
{
    return rx_opened_on(rxs.frequency, tid, sn, slot, pn);
}

/** A frame of TID 0 the radio opened; as rx_opened_tid. */
static int rx_opened(unsigned sn, int slot, uint64_t pn)
{
    return rx_opened_tid(0, sn, slot, pn);
}

/** A pairwise key replaced with frames under way: one the radio opened
 * under the old slot must not move the new key's counter, whose frames
 * start again from 1, and the reordering buffers empty first, under the
 * old key. */
static void check_rekey_slot(void)
{
    uint8_t f[64];
    unsigned len, n;

    setup(1);
    takes_keys = 1;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    len = addba_req(f, 0, 8, 100);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    CHECK(rx_opened(100, 0, 440) == 0 && n_got == 1);
    /* 101 is lost; 102 and 103 wait for it. */
    CHECK(rx_opened(102, 0, 442) == 0 && rx_opened(103, 0, 443) == 0);
    CHECK(n_got == 1 && d->ba[0].stored == 2);
    wlan_ba_rx_flush(d);
    CHECK(n_got == 3 && got_sn(1) == 102 && got_sn(2) == 103);
    CHECK(d->ba[0].active && d->ba[0].stored == 0 && d->ba[0].head == 104);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[0] == 443);

    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0);
    CHECK(wlan_data_pairwise_key(d, 0)->key.hw_slot == 1);
    n = counters.rx_dropped;
    CHECK(rx_opened(104, 0, 444) == -ESTALE && n_got == 3);
    CHECK(counters.rx_dropped == n + 1 && counters.rx_replays == 0);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[0] == 0);
    /* The AP's queue, the old count under the new key, moves no counter. */
    CHECK(rx_opened(105, 1, 445) == -ESTALE && n_got == 3);
    CHECK(rx_opened(106, 1, 446) == -ESTALE && n_got == 3);
    CHECK(counters.rx_dropped == n + 3 && counters.rx_replays == 0);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[0] == 0);
    CHECK(rx_opened(107, 1, 1) == 0 && n_got == 4);
    CHECK(rx_opened(108, 1, 2) == 0 && n_got == 5);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[0] == 2);
    /* A true replay under the new key stays one. */
    CHECK(rx_opened(109, 1, 2) == -ESTALE && counters.rx_replays == 1);
    /* Once the AP's queue has gone, a number that far on is the new key's. */
    now += WLAN_DATA_REKEY_SETTLE;
    CHECK(rx_opened(110, 1, 446) == 0 && n_got == 6);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[0] == 446);
}

/** Whether sent frame `i` is a DELBA of `params` and `reason`. */
static int sent_delba(unsigned i, unsigned params, unsigned reason)
{
    const uint8_t *p = sent[i].frame + 24;

    return p[0] == WLAN_CATEGORY_BACK && p[1] == WLAN_ACTION_DELBA && p[2] == (params & 0xff) &&
           p[3] == params >> 8 && p[4] == reason;
}

/** A pairwise key in the radio replaced ends the sessions each way with a
 * DELBA, as ieee80211_pairwise_rekey does. A first key, the same key again
 * and a software key end none, and a request under way is forgotten. */
static void check_rekey_sessions(void)
{
    uint8_t f[64];
    unsigned len;

    setup(1);
    takes_keys = 1;
    len = addba_req(f, 0, 8, 100);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0 && d->ba[0].active);
    d->ba_tx[0].state = WLAN_BA_TX_ACTIVE;
    d->ba_tx[3].state = WLAN_BA_TX_REQUESTED;
    d->ba_tx[5].retry_at = now + 1000; /* a refusal's wait stands */
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(d->ba[0].active && d->ba_tx[0].state == WLAN_BA_TX_ACTIVE);
    n_sent = 0;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(n_sent == 0 && d->ba[0].active);

    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0);
    CHECK(n_sent == 2 && sent_delba(0, 0x0000, 36) && sent_delba(1, 0x0800, 37));
    CHECK(!d->ba[0].active && d->ba_tx[0].state == WLAN_BA_TX_IDLE);
    CHECK(d->ba_tx[0].retry_at == 0);
    CHECK(d->ba_tx[3].state == WLAN_BA_TX_IDLE && d->ba_tx[3].retry_at == 0);
    CHECK(d->ba_tx[5].retry_at == now + 1000);

    setup(1);
    len = addba_req(f, 0, 8, 100);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0);
    CHECK(d->ba[0].active);

    /* A full queue holds the key and the session until the DELBA goes. */
    setup(1);
    takes_keys = 1;
    len = addba_req(f, 0, 8, 100);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    n_sent = 0;
    radio_full = 1;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == -EINPROGRESS);
    CHECK(d->ba[0].active && wlan_data_pairwise_key(d, 0)->key.hw_slot == 0);
    now += WLAN_DATA_REKEY_WAIT - 1;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == -EINPROGRESS);
    radio_full = 0;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0);
    CHECK(n_sent == 1 && sent_delba(0, 0x0000, 36) && !d->ba[0].active);
    CHECK(wlan_data_pairwise_key(d, 0)->key.hw_slot == 1 && d->ba_stop_due == 0);

    /* It holds them no longer than the limit. */
    setup(1);
    takes_keys = 1;
    len = addba_req(f, 0, 8, 100);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    radio_full = 1;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == -EINPROGRESS);
    now += WLAN_DATA_REKEY_WAIT;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0);
    radio_full = 0;
    CHECK(!d->ba[0].active && wlan_data_pairwise_key(d, 0)->key.hw_slot == 1);
}

/** The sender's count is shared across TIDs: a TID's first frame under the
 * new key follows the count that another TID has carried, and is taken. */
static void check_rekey_tids(void)
{
    unsigned i, n;

    setup(1);
    takes_keys = 1;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(rx_opened_tid(0, 100, 0, 443) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0);
    for (i = 1; i < 200; ++i)
        CHECK(rx_opened_tid(1, i, 1, i) == 0);
    n = n_got;
    CHECK(d->sealed == now);
    d->sealed = 0;
    CHECK(rx_sn(102) == -EACCES && d->sealed == 0); /* in clear proves nothing */
    CHECK(rx_opened_tid(0, 101, 1, 200) == 0 && n_got == n + 1);
    CHECK(d->sealed == now);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[0] == 200);
    CHECK(wlan_data_pairwise_key(d, 0)->key.rx_pn[1] == 199);
}

/** Write an EAPOL-Key payload of `info` at `body`. */
static void key_body(uint8_t *body, unsigned len, unsigned info)
{
    k_memset(body, 0, len);
    k_memcpy(body, eapol_snap, 8);
    body[9] = 3;
    body[12] = 2;
    body[13] = (uint8_t)(info >> 8);
    body[14] = (uint8_t)info;
}

/** Key frame `info` from the AP, opened by the radio under slot 0. */
static int rx_key(unsigned sn, uint64_t pn, unsigned info)
{
    uint8_t f[200], body[40];
    struct wlan_rx_status hw = rxs;
    unsigned len;

    hw.flags = WLAN_RX_DECRYPTED | WLAN_RX_IV_STRIPPED | WLAN_RX_MIC_STRIPPED;
    hw.hw_key = 0;
    hw.pn = pn;
    key_body(body, sizeof(body), info);
    len = ap_frame(f, sta, 1, 0, sn, 0, 0, body, sizeof(body));
    return wlan_data_rx(d, f, len, &hw);
}

/** Key frame `info` from the AP in clear. */
static int rx_clear_key(unsigned sn, unsigned info)
{
    uint8_t f[200], body[40];
    unsigned len;

    key_body(body, sizeof(body), info);
    len = ap_frame(f, sta, 1, 0, sn, 0, 0, body, sizeof(body));
    return wlan_data_rx(d, f, len, &rxs);
}

/** Message 1 on a connection with a pairwise key starts the wait for the
 * renewed key, and its repeats leave the tick; a group key's message, or the
 * later messages with their MIC, do not, and the new key ends it. */
static void check_renewal_wait(void)
{
    uint64_t began;

    setup(1);
    takes_keys = 1;
    CHECK(rx_clear_key(100, 0x008a) == 0 && n_got == 1 && d->renewing == 0); /* a first join */
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(rx_clear_key(105, 0x008a) == 0 && d->renewing == 0); /* forged, in clear */
    CHECK(rx_key(101, 1, 0x1382) == 0 && d->renewing == 0);    /* group message 1 */
    CHECK(rx_key(102, 2, 0x13ca) == 0 && d->renewing == 0);    /* message 3 */
    CHECK(rx_key(103, 3, 0x008a) == 0 && d->renewing == now);
    began = now;
    now += 1000;
    CHECK(rx_key(104, 4, 0x008a) == 0 && d->renewing == began);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, gtk1, 16, 0) == 0 && d->renewing == 0);
}

/** A frame opened under a key proves the AP on the connection's channel
 * from its reception: one the reordering buffer held across a channel move
 * proves nothing there, nor does one heard on the old channel after it; one
 * received on the new channel does. */
static void check_sealed_move(void)
{
    uint8_t f[64];
    unsigned len;
    uint64_t moved;

    setup(1);
    takes_keys = 1;
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    len = addba_req(f, 0, 8, 100);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    CHECK(rx_opened(100, 0, 1) == 0 && n_got == 1 && d->sealed == now);
    /* 101 is lost; 102 waits for it from before the move. */
    now += 10;
    CHECK(rx_opened(102, 0, 3) == 0 && n_got == 1 && d->ba[0].stored == 1);
    now += 10;
    moved = now;
    wlan_data_set_frequency(d, 2437);
    now += 10;
    /* The BlockAckReq gives up on 101 and lets 102 go. */
    k_memset(f, 0, 20);
    f[0] = IEEE80211_FTYPE_CTL | IEEE80211_STYPE_BACK_REQ;
    k_memcpy(f + 4, sta, 6);
    k_memcpy(f + 10, bssid, 6);
    f[16] = 0x04; /* compressed bitmap, TID 0 */
    f[18] = (uint8_t)(103 << 4);
    f[19] = (uint8_t)(103 >> 4);
    CHECK(rx(f, 20) == 0 && n_got == 2 && got_sn(1) == 102 && d->ba[0].stored == 0);
    CHECK(d->sealed < moved);
    now += 10;
    CHECK(rx_opened_on(2412, 0, 103, 0, 4) == 0 && n_got == 3 && d->sealed < moved);
    now += 10;
    CHECK(rx_opened_on(2437, 0, 104, 0, 5) == 0 && n_got == 4 && d->sealed == now);
}

/** BlockAck: ADDBA answered, reordering, BlockAckReq, a moved window, the
 * timeout, DELBA, and refusals. */
static void check_blockack(void)
{
    uint8_t f[64];
    unsigned len;

    setup(1);
    d->privacy = 0;
    ampdu_starts = ampdu_stops = 0;
    len = addba_req(f, 0, 8, 100);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    CHECK(n_sent == 1 && ampdu_starts == 1 && d->ba[0].active && d->ba[0].window == 8);
    CHECK(sent[0].frame[24] == WLAN_CATEGORY_BACK && sent[0].frame[25] == WLAN_ACTION_ADDBA_RESP);
    CHECK(sent[0].frame[26] == 7 && sent[0].frame[27] == 0 && sent[0].frame[28] == 0);
    CHECK(sent[0].frame[29] == 0x02 && sent[0].frame[30] == 0x02); /* TID 0, 8, immediate */
    CHECK(!cmp(sent[0].frame + 4, bssid, 6));
    CHECK(sent[0].info.cookie & WLAN_DATA_COOKIE);

    CHECK(rx_sn(100) == 0 && n_got == 1);
    CHECK(rx_sn(102) == 0 && n_got == 1 && d->ba[0].stored == 1);
    CHECK(rx_sn(101) == 0 && n_got == 3 && got_sn(1) == 101 && got_sn(2) == 102);
    CHECK(rx_sn(101) == -EALREADY && counters.rx_duplicates == 1);
    CHECK(rx_sn(105) == 0 && n_got == 3);
    CHECK(rx_sn(105) == -EALREADY);
    /* The BlockAckReq gives up on 103 and 104. */
    k_memset(f, 0, 20);
    f[0] = IEEE80211_FTYPE_CTL | IEEE80211_STYPE_BACK_REQ;
    k_memcpy(f + 4, sta, 6);
    k_memcpy(f + 10, bssid, 6);
    f[16] = 0x04; /* compressed bitmap, TID 0 */
    f[18] = (uint8_t)(106 << 4);
    f[19] = (uint8_t)(106 >> 4);
    CHECK(rx(f, 20) == 0 && n_got == 4 && got_sn(3) == 105 && d->ba[0].head == 106);
    /* A frame past the window moves it, handing on what it passes. */
    CHECK(rx_sn(108) == 0 && rx_sn(110) == 0 && n_got == 4);
    CHECK(rx_sn(120) == 0);
    CHECK(n_got == 6 && got_sn(4) == 108 && got_sn(5) == 110 && d->ba[0].head == 113);
    /* The hole before 120 times out. */
    CHECK(d->ba[0].stored == 1);
    now += WLAN_BA_TIMEOUT - 1;
    run_work();
    CHECK(n_got == 6);
    now += 1;
    run_work();
    CHECK(n_got == 7 && got_sn(6) == 120 && d->ba[0].head == 121 && d->ba[0].stored == 0);
    /* Sequence numbers wrap. */
    len = addba_req(f, 0, 64, 4094);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0 && d->ba[0].head == 4094);
    CHECK(rx_sn(0) == 0 && rx_sn(4095) == 0 && n_got == 7);
    CHECK(rx_sn(4094) == 0 && n_got == 10 && got_sn(9) == 0);
    /* The AP ends the session; what follows goes on unordered. */
    CHECK(rx_sn(3) == 0 && n_got == 10);
    k_memset(f, 0, 30);
    f[0] = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION;
    k_memcpy(f + 4, sta, 6);
    k_memcpy(f + 10, bssid, 6);
    k_memcpy(f + 16, bssid, 6);
    f[24] = WLAN_CATEGORY_BACK;
    f[25] = WLAN_ACTION_DELBA;
    f[27] = 0x08; /* initiator, TID 0 */
    CHECK(wlan_data_rx_action(d, f, 30, 0) == 0);
    CHECK(!d->ba[0].active && n_got == 11 && got_sn(10) == 3 && ampdu_stops >= 1);
    CHECK(rx_sn(2) == 0 && n_got == 12);

    /* Refused: delayed BlockAck, and any session without QoS. */
    n_sent = 0;
    len = addba_req(f, 1, 8, 0);
    f[27] &= ~0x02;
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0 && !d->ba[1].active);
    CHECK(n_sent == 1 && sent[0].frame[27] == WLAN_STATUS_REQUEST_DECLINED);
    d->qos = 0;
    len = addba_req(f, 1, 8, 0);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0 && !d->ba[1].active);
    d->qos = 1;
    d->mfp = 1;
    CHECK(wlan_data_rx_action(d, f, len, 0) == -EACCES);
    d->mfp = 0;
    f[24] = 4;
    CHECK(wlan_data_rx_action(d, f, len, 0) == -EOPNOTSUPP);
    /* A disconnection drops what a session holds. */
    len = addba_req(f, 0, 8, 10);
    CHECK(wlan_data_rx_action(d, f, len, 0) == 0);
    CHECK(rx_sn(12) == 0 && d->ba[0].stored == 1);
    wlan_data_disconnect(d);
    CHECK(!d->ba[0].active && d->ba[0].stored == 0);
}

/** Build a management frame from the AP of `stype` to `da`, with `n`
 * body bytes. */
static unsigned ap_mgmt(uint8_t *f, unsigned stype, const uint8_t *da, unsigned n)
{
    unsigned i;

    k_memset(f, 0, 24);
    f[0] = (uint8_t)(IEEE80211_FTYPE_MGMT | stype);
    k_memcpy(f + 4, da, 6);
    k_memcpy(f + 10, bssid, 6);
    k_memcpy(f + 16, bssid, 6);
    for (i = 0; i < n; ++i)
        f[24 + i] = (uint8_t)(i + 1);
    return 24 + n;
}

/** BIP on group-addressed frames, CCMP on individually addressed ones, in
 * both directions. */
static void check_mgmt(void)
{
    uint8_t f[256], g[256];
    unsigned len, glen, keyid;
    uint64_t pn, cookie = 0;

    setup(1);
    len = ap_mgmt(f, IEEE80211_STYPE_DEAUTH, bcast, 2);
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == WLAN_DATA_UNPROTECTED && len == 26);
    CHECK(install(WLAN_CIPHER_SUITE_CCMP, 0, 1, tk, 16, 0) == 0);
    CHECK(install(WLAN_CIPHER_SUITE_AES_CMAC, 4, 0, igtk, 16, 0) == 0);
    CHECK(wlan_data_default_key(d, 4, 1) == 0);
    /* A broadcast deauthentication signed with BIP. */
    len = ap_mgmt(f, IEEE80211_STYPE_DEAUTH, bcast, 2);
    CHECK(wlan_bip_sign(&ap_igtk, f, &len, sizeof(f), 7, 4) == 0);
    k_memcpy(g, f, len);
    glen = len;
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == WLAN_DATA_PROTECTED && len == 26);
    CHECK(wlan_data_rx_mgmt(d, g, &glen, &rxs) == -ESTALE);
    len = ap_mgmt(f, IEEE80211_STYPE_DEAUTH, bcast, 2);
    CHECK(wlan_bip_sign(&ap_igtk, f, &len, sizeof(f), 8, 4) == 0);
    f[24] ^= 1;
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == -EBADMSG);
    len = ap_mgmt(f, IEEE80211_STYPE_DEAUTH, bcast, 2);
    CHECK(wlan_bip_sign(&ap_igtk, f, &len, sizeof(f), 9, 5) == 0);
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == -ENOKEY);
    len = ap_mgmt(f, IEEE80211_STYPE_DEAUTH, bcast, 2);
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == WLAN_DATA_UNPROTECTED);
    /* A beacon is no business of this layer's. */
    len = ap_mgmt(f, IEEE80211_STYPE_BEACON, bcast, 12);
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == WLAN_DATA_UNPROTECTED && len == 36);
    /* An individually addressed protected deauthentication. */
    len = ap_mgmt(f, IEEE80211_STYPE_DEAUTH, sta, 2);
    ap_ccmp(&ap_ptk, f, &len, 3, 0);
    k_memcpy(g, f, len);
    glen = len;
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == WLAN_DATA_PROTECTED && len == 26);
    CHECK(f[24] == 1 && f[25] == 2 && !(f[1] & (IEEE80211_FCTL_PROTECTED >> 8)));
    CHECK(wlan_data_rx_mgmt(d, g, &glen, &rxs) == -ESTALE);
    /* Management frames keep their own counter: data at a lower PN passes. */
    CHECK(wlan_data_pairwise_key(d, 0)->rx_mgmt_pn == 3);
    /* A protected probe response is no robust frame. */
    len = ap_mgmt(f, IEEE80211_STYPE_PROBE_RESP, sta, 12);
    ap_ccmp(&ap_ptk, f, &len, 4, 0);
    CHECK(wlan_data_rx_mgmt(d, f, &len, &rxs) == -EINVAL);

    /* Robust frames by type and action category. */
    len = ap_mgmt(f, IEEE80211_STYPE_ACTION, sta, 4);
    f[24] = WLAN_CATEGORY_SA_QUERY;
    CHECK(wlan_data_robust_mgmt(f, len));
    f[24] = 4; /* Public */
    CHECK(!wlan_data_robust_mgmt(f, len));
    f[24] = 127;
    CHECK(!wlan_data_robust_mgmt(f, len));
    len = ap_mgmt(f, IEEE80211_STYPE_AUTH, sta, 6);
    CHECK(!wlan_data_robust_mgmt(f, len));

    /* The station's protected frames, as the AP checks them. */
    len = ap_mgmt(f, IEEE80211_STYPE_ACTION, bssid, 4);
    k_memcpy(f + 10, sta, 6);
    f[24] = WLAN_CATEGORY_SA_QUERY;
    CHECK(wlan_data_tx_mgmt(d, f, len, sizeof(f), 0, 1, &cookie) == 0);
    CHECK(cookie & WLAN_DATA_COOKIE && sent[0].len == len + 16);
    CHECK(sent[0].info.rates[0].index == 0 && sent[0].info.rates[1].index == -1);
    glen = sent[0].len;
    CHECK(wlan_ccmp_decrypt(&ap_ptk, sent[0].frame, &glen, &pn, &keyid) == 0);
    CHECK(glen == len && pn == 1 && sent[0].frame[24] == WLAN_CATEGORY_SA_QUERY);
    len = ap_mgmt(f, IEEE80211_STYPE_DEAUTH, bcast, 2);
    k_memcpy(f + 10, sta, 6);
    CHECK(wlan_data_tx_mgmt(d, f, len, sizeof(f), 2437, 1, 0) == 0);
    CHECK(sent[1].info.frequency == 2437 && (sent[1].info.flags & WLAN_TX_NO_ACK));
    glen = sent[1].len;
    CHECK(wlan_bip_check(&ap_igtk, sent[1].frame, &glen, &pn, &keyid) == 0);
    CHECK(pn == 1 && keyid == 4 && glen == len);
    /* Unprotected, the frame is sent as given; a missing key refuses. */
    len = ap_mgmt(f, IEEE80211_STYPE_AUTH, bssid, 6);
    CHECK(wlan_data_tx_mgmt(d, f, len, sizeof(f), 0, 0, 0) == 0 && sent[2].len == len);
    CHECK(sent[2].frame[22] == 0x20); /* the third non-QoS sequence number */
    wlan_data_clear_keys(d);
    CHECK(wlan_data_tx_mgmt(d, f, len, sizeof(f), 0, 1, 0) == -ENOKEY);
}

/** An A-MSDU of two subframes, and one forged from a plain MSDU. */
static void check_amsdu(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN], body[200];
    unsigned len, n = 0;

    setup(1);
    d->privacy = 0;
    k_memcpy(body + n, sta, 6);
    k_memcpy(body + n + 6, far, 6);
    body[n + 12] = 0;
    body[n + 13] = 30;
    payload(body + n + 14, 30, 1);
    n += 44; /* padded to 4 */
    k_memcpy(body + n, bcast, 6);
    k_memcpy(body + n + 6, far, 6);
    body[n + 12] = 0;
    body[n + 13] = 20;
    payload(body + n + 14, 20, 2);
    n += 34;
    len = ap_frame(f, sta, 1, 0, 70, 0, 0, body, n);
    f[24] |= IEEE80211_QOS_CTL_A_MSDU_PRESENT;
    CHECK(rx(f, len) == 0 && n_got == 2);
    CHECK(got_len[0] == 24 + 30 && got_len[1] == 24 + 20);
    CHECK(!cmp(got[0] + 4, sta, 6) && !cmp(got[1] + 4, bcast, 6));
    CHECK(!cmp(got[0] + 16, far, 6) && got[0][0] == IEEE80211_FTYPE_DATA);
    CHECK(!cmp(got[0] + 24, body + 14, 30));
    /* A plain MSDU whose A-MSDU bit was set: its first DA is LLC/SNAP. */
    payload(body, 60, 5);
    len = ap_frame(f, sta, 1, 0, 71, 0, 0, body, 60);
    f[24] |= IEEE80211_QOS_CTL_A_MSDU_PRESENT;
    CHECK(rx(f, len) == -EINVAL && n_got == 2);
}

/** Rate selection by a radio's own algorithm, and transmit status. */
static void check_rates(void)
{
    uint8_t f[200];
    unsigned len;
    struct wlan_tx_status st;

    setup(0);
    CHECK(wlan_data_connect(d, &bss, 0, 0, 0) == 0);
    radio.rate_ops = &rc_ops;
    rate_selects = rate_statuses = 0;
    len = sta_frame(f, ipv4_snap, 60);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == 0);
    CHECK(rate_selects == 1 && sent[0].info.rates[0].index == 11);
    /* EAPOL still takes the lowest basic rate. */
    len = sta_frame(f, eapol_snap, 60);
    CHECK(wlan_data_tx(d, f, len, sizeof(f), 0) == 0);
    CHECK(rate_selects == 1 && sent[1].info.rates[0].index == 0);
    k_memset(&st, 0, sizeof(st));
    st.cookie = sent[0].info.cookie;
    st.rates[0].index = 11;
    st.rates[0].count = 3;
    st.rates[1].index = -1;
    wlan_data_tx_status(d, &st);
    CHECK(rate_statuses == 1 && counters.tx_retries == 2 && counters.tx_failed == 1);
    st.flags = WLAN_TX_ACKED;
    st.rates[0].count = 1;
    wlan_data_tx_status(d, &st);
    CHECK(rate_statuses == 2 && counters.tx_retries == 2 && counters.tx_failed == 1);
    st.cookie = 5; /* not the data path's */
    wlan_data_tx_status(d, &st);
    CHECK(rate_statuses == 2);
    radio.rate_ops = 0;
    /* A BSS with no rate the radio knows falls back to the radio's lowest. */
    bss.ies_len = 0;
    CHECK(wlan_data_connect(d, &bss, 0, 0, 0) == 0);
    CHECK(d->rates_basic == 1);
    /* Priorities and access categories. */
    k_memset(f, 0, 20);
    f[12] = 0x08;
    f[15] = 0xb8; /* DSCP 46, EF */
    CHECK(wlan_data_priority(f, 20) == 5 && wlan_data_ac(5) == WLAN_AC_VI);
    f[12] = 0x86;
    f[13] = 0xdd;
    f[14] = 0x6e; /* traffic class 0xe0 */
    CHECK(wlan_data_priority(f, 20) == 7);
    f[12] = 0x88;
    f[13] = 0x8e;
    CHECK(wlan_data_priority(f, 20) == 7 && wlan_data_ac(3) == WLAN_AC_BE);
}

/** Main. */
int main(void)
{
    check_keys();
    check_tx_ccmp();
    check_tx_nokey();
    check_tx_hw();
    check_rx_ccmp();
    check_fragments();
    check_tkip();
    check_rekey();
    check_blockack();
    check_rekey_slot();
    check_rekey_sessions();
    check_rekey_tids();
    check_renewal_wait();
    check_sealed_move();
    check_mgmt();
    check_amsdu();
    check_rates();
    if (d) {
        wlan_data_disconnect(d);
        kfree(d);
    }
    if (failures) {
        printf("wlan_data_hosttest: %u failures\n", failures);
        return 1;
    }
    printf("wlan_data_hosttest: ok\n");
    return 0;
}
