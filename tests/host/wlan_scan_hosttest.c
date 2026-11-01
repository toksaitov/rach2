/* Check the wireless worker's queue, the scan and the BSS table against a
 * stub radio and scripted access points that beacon and answer probes.
 * host-deps: rach2/sys/network/wireless/work.c rach2/sys/network/wireless/scan.c
 * host-deps: rach2/sys/network/wireless/bss.c rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 */
#include "../../rach2/sys/network/wireless/work.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kirq.h"
#include "../../rach2/sys/lib/kstring.h"

extern int printf(const char *format, ...);
extern unsigned long kheap_host_largest;

static unsigned failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            printf("wlan_scan_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

/* ---- the machine around the code ---- */

static uint64_t now = 1000;
static uint32_t irq_flag = 1;

uint64_t tick_count(void)
{
    return now;
}

uint32_t kirq_save(void)
{
    uint32_t flags = irq_flag;

    irq_flag = 0;
    return flags;
}

void kirq_restore(uint32_t flags)
{
    irq_flag = flags;
}

/** Whether a and b hold the same n bytes. */
static int same(const void *a, const void *b, unsigned n)
{
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;

    while (n--) {
        if (*p++ != *q++) {
            return 0;
        }
    }
    return 1;
}

/* ---- the events the scan reports ---- */

static struct wlan_event events[32];
static unsigned n_events;

static void record_event(struct wlan *wlan, const struct wlan_event *event)
{
    (void)wlan;
    if (n_events < 32) {
        events[n_events] = *event;
    }
    n_events++;
}

/* ---- the radio ---- */

#define MAX_TX 64

static struct wlan_radio radio;
static struct wlan_band band; /* the band after the regulatory rules */
static const struct wlan_channel *tuned;
static unsigned tuned_filters;
static uint16_t configs[64];
static unsigned n_configs;
static struct {
    uint8_t frame[1024];
    unsigned len;
    struct wlan_tx_info info;
} txs[MAX_TX];
static unsigned n_tx;
static int hw_result = -EOPNOTSUPP;
static struct wlan_scan_request hw_request;
static unsigned hw_starts, hw_cancels;
static struct wlan_scan scan;
static struct wlan_bss_table table;

static int radio_start(struct wlan_radio *r)
{
    (void)r;
    return 0;
}

static void radio_stop(struct wlan_radio *r)
{
    (void)r;
}

static int radio_config(struct wlan_radio *r, const struct wlan_channel *channel, unsigned filters)
{
    (void)r;
    tuned = channel;
    tuned_filters = filters;
    if (n_configs < 64) {
        configs[n_configs] = channel->frequency;
    }
    n_configs++;
    return 0;
}

static void ap_heard_probe(const uint8_t *frame, unsigned len, uint16_t frequency);

static int radio_tx(struct wlan_radio *r, const uint8_t *frame, unsigned len,
                    const struct wlan_tx_info *info)
{
    (void)r;
    if (n_tx < MAX_TX && len <= sizeof(txs[0].frame)) {
        k_memcpy(txs[n_tx].frame, frame, len);
        txs[n_tx].len = len;
        txs[n_tx].info = *info;
    }
    n_tx++;
    ap_heard_probe(frame, len, info->frequency);
    return 0;
}

static int radio_scan_start(struct wlan_radio *r, const struct wlan_scan_request *request)
{
    (void)r;
    hw_starts++;
    hw_request = *request;
    return hw_result;
}

static void radio_scan_cancel(struct wlan_radio *r)
{
    (void)r;
    hw_cancels++;
    wlan_scan_hw_done(&scan, 1);
}

static struct wlan_radio_ops soft_ops = {
    radio_start, radio_stop, radio_config, radio_tx, 0, 0, 0, 0, 0, 0};
static struct wlan_radio_ops hard_ops = {
    radio_start, radio_stop, radio_config,     radio_tx,          0,
    0,           0,          radio_scan_start, radio_scan_cancel, 0};

/** The world regulatory domain as task 6 narrows a radio's band: channels
 * 1 to 11 active, 12 and 13 passive, 14 off. */
static void make_band(void)
{
    static const uint16_t rates[12] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};
    unsigned i;

    k_memset(&band, 0, sizeof(band));
    for (i = 0; i < 13; ++i) {
        band.channels[i].frequency = (uint16_t)(2412 + 5 * i);
        band.channels[i].number = (uint8_t)(i + 1);
        band.channels[i].max_power = 20;
    }
    band.channels[11].flags = WLAN_CHANNEL_PASSIVE;
    band.channels[12].flags = WLAN_CHANNEL_PASSIVE;
    band.channels[13].frequency = 2484;
    band.channels[13].number = 14;
    band.channels[13].flags = WLAN_CHANNEL_DISABLED;
    band.n_channels = 14;
    k_memcpy(band.rates, rates, sizeof(rates));
    band.n_rates = 12;
    band.ht_supported = 1;
    band.ht_cap = 0x012c;
    band.ampdu_factor = 3;
    band.ampdu_density = 6;
    band.mcs[0] = 0xff;
}

/** A fresh radio, table and scan with the given operations. */
static void setup(const struct wlan_radio_ops *ops)
{
    k_memset(&radio, 0, sizeof(radio));
    radio.ops = ops;
    radio.mac[0] = 0x02;
    radio.mac[5] = 0x01;
    make_band();
    radio.band = band;
    wlan_bss_destroy(&table);
    wlan_bss_init(&table);
    wlan_scan_attach(&scan, (struct wlan *)&radio, record_event, &radio, &band, &table);
    tuned = 0;
    tuned_filters = 0;
    n_configs = n_tx = n_events = 0;
    hw_starts = hw_cancels = 0;
    hw_result = -EOPNOTSUPP;
}

/* ---- the access points ---- */

struct ap {
    uint8_t bssid[6];
    const char *ssid;
    int hidden;      /* beacons carry an empty SSID; answers only directed probes */
    unsigned number; /* channel */
    int16_t signal;  /* dBm */
    unsigned phase;  /* beacons at now % 100 == phase */
    uint64_t answer; /* the tick a probe response is due, 0 for none */
};

static struct ap aps[4] = {
    {{0x0a, 0, 0, 0, 0, 1}, "lab", 0, 1, -40, 50, 0},
    {{0x0a, 0, 0, 0, 0, 2}, "hidden", 1, 6, -55, 60, 0},
    {{0x0a, 0, 0, 0, 0, 3}, "far", 0, 12, -80, 70, 0},
    {{0x0a, 0, 0, 0, 0, 4}, "japan", 0, 14, -30, 10, 0},
};

static unsigned slen(const char *s)
{
    unsigned n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Build a beacon or probe response of `ap` into frame; returns its length. */
static unsigned ap_frame(const struct ap *ap, int beacon, uint8_t *frame)
{
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96};
    unsigned used, n = ap->hidden && beacon ? 0 : slen(ap->ssid);
    uint8_t ds = (uint8_t)ap->number;
    uint64_t tsf = now * 1000;
    unsigned i;

    k_memset(frame, 0, 36);
    frame[0] = beacon ? 0x80 : 0x50;
    k_memset(frame + 4, 0xff, 6);
    k_memcpy(frame + 10, ap->bssid, 6);
    k_memcpy(frame + 16, ap->bssid, 6);
    for (i = 0; i < 8; ++i) {
        frame[24 + i] = (uint8_t)(tsf >> (8 * i));
    }
    frame[32] = 100;
    frame[34] = 0x11;
    frame[35] = 0x04;
    used = 36;
    CHECK(ieee80211_put_elem(frame, 512, &used, WLAN_EID_SSID, (const uint8_t *)ap->ssid, n) == 0);
    CHECK(ieee80211_put_elem(frame, 512, &used, WLAN_EID_SUPP_RATES, rates, 4) == 0);
    CHECK(ieee80211_put_elem(frame, 512, &used, WLAN_EID_DS_PARAMS, &ds, 1) == 0);
    return used;
}

/** Deliver a frame of `ap` as rx.c would, at the tuned channel. */
static int ap_deliver(const struct ap *ap, int beacon)
{
    struct wlan_rx_status rx;
    uint8_t frame[512];
    unsigned len = ap_frame(ap, beacon, frame);

    k_memset(&rx, 0, sizeof(rx));
    rx.frequency = tuned->frequency;
    rx.signal = ap->signal;
    rx.hw_key = -1;
    return wlan_bss_rx(&table, &band, frame, len, &rx, now);
}

/** The AP on the probe's channel answers it 2 ms later, if the SSID asked
 * for is its own, or is a wildcard and the AP does not hide. */
static void ap_heard_probe(const uint8_t *frame, unsigned len, uint16_t frequency)
{
    struct ieee80211_elems e;
    const uint8_t *ssid;
    unsigned i;

    CHECK(len >= IEEE80211_HDRLEN);
    CHECK(ieee80211_parse_elems(frame + IEEE80211_HDRLEN, len - IEEE80211_HDRLEN, &e) == 0);
    ssid = e.data[WLAN_EID_SSID];
    if (!ssid) {
        return;
    }
    for (i = 0; i < 4; ++i) {
        if (2407 + 5 * aps[i].number != frequency) {
            continue;
        }
        if (ssid[1] ? ssid[1] == slen(aps[i].ssid) && same(ssid + 2, aps[i].ssid, ssid[1])
                    : !aps[i].hidden) {
            aps[i].answer = now + 2;
        }
    }
}

/** Run the world for `ms` milliseconds: each tick, the worker runs what is
 * due, and the APs on the tuned channel beacon and answer. */
static void advance(unsigned ms)
{
    unsigned i;

    while (ms--) {
        now++;
        (void)wlan_work_run_due(now);
        for (i = 0; i < 4; ++i) {
            if (!tuned || 2407 + 5 * aps[i].number != tuned->frequency) {
                continue;
            }
            if (now % 100 == aps[i].phase) {
                (void)ap_deliver(&aps[i], 1);
            }
            if (aps[i].answer == now) {
                aps[i].answer = 0;
                (void)ap_deliver(&aps[i], 0);
            }
        }
    }
}

/** The number of events of `type` recorded. */
static unsigned count_events(enum wlan_event_type type)
{
    unsigned i, n = 0;

    for (i = 0; i < n_events && i < 32; ++i) {
        n += events[i].type == type;
    }
    return n;
}

/* ---- the worker's queue ---- */

static char order[16];
static unsigned n_order;
static struct wlan_work wa, wb, wc, wd, we;

static void note(struct wlan_work *w)
{
    order[n_order++] = w == &wa ? 'a' : w == &wb ? 'b' : w == &wc ? 'c' : w == &wd ? 'd' : 'e';
}

/** d queues e for now, which runs in the same pass, and tries itself. */
static void note_and_queue(struct wlan_work *w)
{
    note(w);
    CHECK(wlan_queue_work(w, 0) == -EBUSY);
    CHECK(wlan_queue_work(&we, now) == 0);
}

static void queue_order(void)
{
    wa.run = wb.run = wc.run = we.run = note;
    wd.run = note_and_queue;
    n_order = 0;
    CHECK(wlan_work_next_due() == 0);
    CHECK(wlan_queue_work(&wa, now + 5) == 0);
    CHECK(wlan_queue_work(&wb, now + 3) == 0);
    CHECK(wlan_queue_work(&wc, now + 3) == 0);
    CHECK(wlan_work_next_due() == now + 3);
    CHECK(wlan_queue_work(&wd, 0) == 0);
    CHECK(wlan_work_next_due() == 1);
    CHECK(wlan_queue_work(&wb, now + 1) == -EBUSY);
    CHECK(wlan_queue_work(0, 0) == -EINVAL);
    CHECK(wlan_work_run_due(now + 4) == 4);
    CHECK(n_order == 4 && same(order, "debc", 4));
    CHECK(wlan_work_next_due() == now + 5);
    wlan_cancel_work(&wa);
    CHECK(!wa.pending && wlan_work_next_due() == 0);
    CHECK(wlan_work_run_due(now + 100) == 0);
    wlan_cancel_work(&wa);
    CHECK(wlan_queue_work(&wa, now) == 0);
    CHECK(wlan_work_run_due(now) == 1 && order[4] == 'a');
}

/* ---- scans ---- */

/** Check the probe request at txs[i]: its addresses, SSID, DS channel,
 * rates, HT capabilities, extra element and transmit info. */
static void check_probe(unsigned i, const char *ssid, uint16_t frequency)
{
    static const uint8_t ff[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t *f = txs[i].frame;
    struct ieee80211_elems e;
    unsigned n = slen(ssid);

    CHECK(f[0] == 0x40 && f[1] == 0);
    CHECK(same(f + 4, ff, 6) && same(f + 10, radio.mac, 6) && same(f + 16, ff, 6));
    CHECK(ieee80211_parse_elems(f + 24, txs[i].len - 24, &e) == 0);
    CHECK(e.data[WLAN_EID_SSID] && e.data[WLAN_EID_SSID][1] == n &&
          same(e.data[WLAN_EID_SSID] + 2, ssid, n));
    CHECK(e.data[WLAN_EID_SUPP_RATES] && e.data[WLAN_EID_SUPP_RATES][1] == 8 &&
          e.data[WLAN_EID_SUPP_RATES][2] == 2 && e.data[WLAN_EID_SUPP_RATES][5] == 22);
    CHECK(e.data[WLAN_EID_EXT_SUPP_RATES] && e.data[WLAN_EID_EXT_SUPP_RATES][1] == 4 &&
          e.data[WLAN_EID_EXT_SUPP_RATES][5] == 108);
    CHECK(e.data[WLAN_EID_DS_PARAMS] && 2407u + 5u * e.data[WLAN_EID_DS_PARAMS][2] == frequency);
    CHECK(e.data[WLAN_EID_HT_CAPABILITY] && e.data[WLAN_EID_HT_CAPABILITY][2] == 0x2c &&
          e.data[WLAN_EID_HT_CAPABILITY][3] == 0x01 && e.data[WLAN_EID_HT_CAPABILITY][4] == 0x1b &&
          e.data[WLAN_EID_HT_CAPABILITY][5] == 0xff);
    CHECK(e.data[WLAN_EID_EXT_CAPABILITY] && f[txs[i].len - 3] == WLAN_EID_EXT_CAPABILITY);
    CHECK(txs[i].info.frequency == frequency && (txs[i].info.cookie & WLAN_SCAN_COOKIE));
    CHECK(txs[i].info.flags == WLAN_TX_NO_ACK && txs[i].info.hw_key == -1);
    CHECK(txs[i].info.tid == WLAN_NON_QOS_TID && txs[i].info.rates[0].count == 1);
    CHECK(txs[i].info.rates[1].index == -1 && txs[i].info.rates[1].count == 0);
}

/** A request for `n` SSIDs, the first `ssid`, with one extra element. */
static void make_request(struct wlan_scan_request *r, unsigned n, const char *ssid)
{
    k_memset(r, 0, sizeof(*r));
    r->n_ssids = n;
    r->ssids[0].len = slen(ssid);
    k_memcpy(r->ssids[0].data, ssid, r->ssids[0].len);
    r->ies[0] = WLAN_EID_EXT_CAPABILITY;
    r->ies[1] = 1;
    r->ies[2] = 0x04;
    r->ies_len = 3;
    r->cookie = 77;
}

/** A wildcard scan of every channel: probes on 1 to 11, listening on 12 and
 * 13, nothing on 14, and a table of what answered and beaconed. */
static void full_scan(void)
{
    struct wlan_scan_request r;
    struct wlan_bss *bss;
    uint64_t t0;
    unsigned i;

    setup(&soft_ops);
    make_request(&r, 1, "");
    t0 = now;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    CHECK(wlan_scan_running(&scan) && n_events == 0);
    CHECK(wlan_scan_start(&scan, &r) == -EBUSY);
    advance(1);
    CHECK(n_events == 1 && events[0].type == WLAN_EVENT_SCAN_STARTED && events[0].cookie == 77);
    advance(11 * WLAN_SCAN_ACTIVE_DWELL_MS + 2 * WLAN_SCAN_PASSIVE_DWELL_MS - 1);
    CHECK(count_events(WLAN_EVENT_SCAN_DONE) == 0);
    advance(1);
    CHECK(n_events == 2 && events[1].type == WLAN_EVENT_SCAN_DONE && events[1].cookie == 77);
    CHECK(now - t0 == 551 && !wlan_scan_running(&scan));
    CHECK(n_configs == 13);
    for (i = 0; i < 13 && i < n_configs; ++i) {
        CHECK(configs[i] == 2412 + 5 * i);
    }
    CHECK(tuned_filters & WLAN_FILTER_OTHER_BSS);
    CHECK(n_tx == 11 && scan.probes_sent == 11 && scan.channels_tuned == 13);
    for (i = 0; i < 11 && i < n_tx; ++i) {
        check_probe(i, "", (uint16_t)(2412 + 5 * i));
    }
    CHECK(table.count == 3);
    bss = wlan_bss_find(&table, aps[0].bssid, 2412);
    CHECK(bss && bss->ssid_len == 3 && same(bss->ssid, "lab", 3) && bss->signal == -4000);
    CHECK(bss && bss->beacon_interval == 100 && bss->capability == 0x0411);
    bss = wlan_bss_find(&table, aps[1].bssid, 0);
    CHECK(bss && bss->frequency == 2437 && bss->ssid_len == 0);
    bss = wlan_bss_find(&table, aps[2].bssid, 2467);
    CHECK(bss && bss->ssid_len == 3 && bss->signal == -8000);
    CHECK(!wlan_bss_find(&table, aps[3].bssid, 0));
    advance(1000);
    CHECK(n_configs == 13 && n_tx == 11 && n_events == 2);
}

/** A directed scan of one channel finds the hidden network's name, which
 * its hidden beacons then keep. */
static void directed_scan(void)
{
    struct wlan_scan_request r;
    struct wlan_bss *bss;
    uint8_t ies[WLAN_MAX_BSS_IES];
    unsigned ies_len;
    uint64_t tsf;

    setup(&soft_ops);
    make_request(&r, 1, "hidden");
    r.n_frequencies = 2;
    r.frequencies[0] = 2437;
    r.frequencies[1] = 2437;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(WLAN_SCAN_ACTIVE_DWELL_MS + 2);
    CHECK(count_events(WLAN_EVENT_SCAN_DONE) == 1);
    CHECK(n_configs == 1 && configs[0] == 2437 && n_tx == 1);
    check_probe(0, "hidden", 2437);
    bss = wlan_bss_find(&table, aps[1].bssid, 2437);
    CHECK(bss && bss->ssid_len == 6 && same(bss->ssid, "hidden", 6));
    if (!bss) {
        return;
    }
    k_memcpy(ies, bss->ies, bss->ies_len);
    ies_len = bss->ies_len;
    tsf = bss->tsf;
    advance(100);
    CHECK(ap_deliver(&aps[1], 1) == 0);
    CHECK(bss->ssid_len == 6 && bss->ies_len == ies_len && same(bss->ies, ies, ies_len));
    CHECK(bss->tsf > tsf && bss->seen == now && table.count == 1);
}

/** No SSIDs: a passive scan sends nothing and dwells long enough for a
 * beacon. */
static void passive_scan(void)
{
    struct wlan_scan_request r;

    setup(&soft_ops);
    make_request(&r, 0, "");
    r.n_frequencies = 2;
    r.frequencies[0] = 2412;
    r.frequencies[1] = 2467;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(2 * WLAN_SCAN_PASSIVE_DWELL_MS + 1);
    CHECK(count_events(WLAN_EVENT_SCAN_DONE) == 1 && n_tx == 0 && n_configs == 2);
    CHECK(wlan_bss_find(&table, aps[0].bssid, 2412) && wlan_bss_find(&table, aps[2].bssid, 2467));
}

/** An abort mid-scan returns to the home channel and reports the abort; an
 * abort before the scan begins reports it without a start. */
static void abort_scan(void)
{
    struct wlan_scan_request r;
    unsigned configs_then;

    setup(&soft_ops);
    wlan_scan_set_home(&scan, &band.channels[5], WLAN_FILTER_CONTROL);
    make_request(&r, 1, "");
    CHECK(wlan_scan_abort(&scan) == -ENOENT);
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(100);
    CHECK(n_configs == 4 && tuned_filters == (WLAN_FILTER_CONTROL | WLAN_FILTER_OTHER_BSS));
    CHECK(wlan_scan_abort(&scan) == 0);
    advance(1);
    CHECK(n_events == 2 && events[1].type == WLAN_EVENT_SCAN_ABORTED && events[1].cookie == 77);
    CHECK(n_configs == 5 && configs[4] == 2437 && tuned_filters == WLAN_FILTER_CONTROL);
    CHECK(!wlan_scan_running(&scan) && wlan_scan_abort(&scan) == -ENOENT);
    configs_then = n_configs;
    advance(1000);
    CHECK(n_configs == configs_then && n_events == 2);

    n_events = 0;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    CHECK(wlan_scan_abort(&scan) == 0);
    advance(1);
    CHECK(n_events == 1 && events[0].type == WLAN_EVENT_SCAN_ABORTED);
    CHECK(n_configs == configs_then && !wlan_scan_running(&scan));
    wlan_scan_set_home(&scan, 0, WLAN_FILTER_CONTROL);
    CHECK(!scan.home && scan.home_filters == 0);
}

/** Requests the scan refuses, each leaving it idle. */
static void refusals(void)
{
    struct wlan_scan_request r;

    setup(&soft_ops);
    make_request(&r, 1, "");
    r.n_frequencies = 1;
    r.frequencies[0] = 2400;
    CHECK(wlan_scan_start(&scan, &r) == -EINVAL);
    r.frequencies[0] = 2484;
    CHECK(wlan_scan_start(&scan, &r) == -EINVAL);
    r.n_frequencies = WLAN_RADIO_MAX_CHANNELS + 1;
    CHECK(wlan_scan_start(&scan, &r) == -EINVAL);
    make_request(&r, 1, "");
    r.ssids[0].len = IEEE80211_MAX_SSID_LEN + 1;
    CHECK(wlan_scan_start(&scan, &r) == -EINVAL);
    make_request(&r, WLAN_MAX_SCAN_SSIDS + 1, "");
    CHECK(wlan_scan_start(&scan, &r) == -EINVAL);
    make_request(&r, 1, "");
    r.ies_len = WLAN_MAX_SCAN_IE + 1;
    CHECK(wlan_scan_start(&scan, &r) == -EINVAL);
    CHECK(wlan_scan_start(&scan, 0) == -EINVAL);
    CHECK(!wlan_scan_running(&scan) && wlan_work_next_due() == 0);
}

/** The radio's own scan: its completion, its cancellation and its refusal. */
static void hardware_scan(void)
{
    struct wlan_scan_request r;

    setup(&hard_ops);
    hw_result = 0;
    make_request(&r, 1, "lab");
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(1);
    CHECK(hw_starts == 1 && n_events == 1 && events[0].type == WLAN_EVENT_SCAN_STARTED);
    CHECK(hw_request.n_frequencies == 13 && hw_request.frequencies[12] == 2472);
    CHECK(hw_request.n_ssids == 1 && hw_request.ssids[0].len == 3 && hw_request.cookie == 77);
    advance(500);
    CHECK(n_events == 1 && n_configs == 0 && n_tx == 0 && wlan_scan_running(&scan));
    wlan_scan_hw_done(&scan, 0);
    wlan_scan_hw_done(&scan, 0);
    advance(1);
    CHECK(n_events == 2 && events[1].type == WLAN_EVENT_SCAN_DONE && !wlan_scan_running(&scan));
    wlan_scan_hw_done(&scan, 0);
    advance(1);
    CHECK(n_events == 2);

    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(1);
    CHECK(wlan_scan_abort(&scan) == 0);
    advance(1);
    CHECK(hw_cancels == 1 && n_events == 4 && events[3].type == WLAN_EVENT_SCAN_ABORTED);
    CHECK(!wlan_scan_running(&scan));

    hw_result = -EIO;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(1);
    CHECK(n_events == 5 && events[4].type == WLAN_EVENT_SCAN_ABORTED && n_configs == 0);

    hw_result = 0;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(1);
    wlan_scan_hw_done(&scan, 1);
    advance(1);
    CHECK(n_events == 7 && events[6].type == WLAN_EVENT_SCAN_ABORTED && hw_cancels == 1);
    wlan_scan_detach(&scan);
}

/* ---- the BSS table ---- */

/** Deliver a hand-made frame at `frequency`. */
static int deliver_raw(const uint8_t *frame, unsigned len, uint16_t frequency, int16_t signal)
{
    struct wlan_rx_status rx;

    k_memset(&rx, 0, sizeof(rx));
    rx.frequency = frequency;
    rx.signal = signal;
    return wlan_bss_rx(&table, &band, frame, len, &rx, now);
}

/** A beacon of BSSID 0a:..:id, SSID "n", DS channel `ds` (0 for none). */
static unsigned raw_beacon(uint8_t *frame, unsigned id, unsigned ds)
{
    unsigned used = 36;
    uint8_t ch = (uint8_t)ds;

    k_memset(frame, 0, 36);
    frame[0] = 0x80;
    k_memset(frame + 4, 0xff, 6);
    frame[10] = frame[16] = 0x0a;
    frame[14] = frame[20] = (uint8_t)(id >> 8);
    frame[15] = frame[21] = (uint8_t)id;
    frame[24] = 0x08;
    frame[31] = 0x01;
    frame[32] = 0x64;
    frame[34] = 0x01;
    CHECK(ieee80211_put_elem(frame, 3000, &used, WLAN_EID_SSID, (const uint8_t *)"n", 1) == 0);
    if (ds) {
        CHECK(ieee80211_put_elem(frame, 3000, &used, WLAN_EID_DS_PARAMS, &ch, 1) == 0);
    }
    return used;
}

static void bss_fields(void)
{
    uint8_t frame[3000];
    unsigned len, cursor, n;
    uint32_t generation;
    struct wlan_bss *bss;

    setup(&soft_ops);
    k_memset(frame, 0, sizeof(frame));
    len = raw_beacon(frame, 1, 0);
    CHECK(deliver_raw(frame, len, 2442, -61) == 0);
    bss = wlan_bss_find(&table, frame + 16, 2442);
    CHECK(bss && bss->signal == -6100 && bss->tsf == 0x0100000000000008ull);
    CHECK(bss && bss->beacon_interval == 100 && bss->capability == 1 && bss->seen == now);
    CHECK(bss && bss->ies_len == len - 36 && same(bss->ies, frame + 36, len - 36));

    len = raw_beacon(frame, 2, 3);
    CHECK(deliver_raw(frame, len, 2427, -70) == 0);
    CHECK(wlan_bss_find(&table, frame + 16, 2422) && !wlan_bss_find(&table, frame + 16, 2427));
    len = raw_beacon(frame, 3, 14);
    CHECK(deliver_raw(frame, len, 2484, -70) == -EINVAL);
    len = raw_beacon(frame, 3, 0);
    CHECK(deliver_raw(frame, len, 2484, -70) == -EINVAL);
    CHECK(deliver_raw(frame, len, 2490, -70) == -EINVAL);
    CHECK(deliver_raw(frame, len - 1, 2412, -70) == -EINVAL);
    CHECK(deliver_raw(frame, 35, 2412, -70) == -EINVAL);
    frame[0] = 0x10;
    CHECK(deliver_raw(frame, len, 2412, -70) == -EINVAL);
    frame[0] = 0x88;
    CHECK(deliver_raw(frame, len, 2412, -70) == -EINVAL);
    frame[0] = 0x81;
    CHECK(deliver_raw(frame, len, 2412, -70) == -EINVAL);
    frame[0] = 0x80;
    frame[36] = WLAN_EID_SUPP_RATES;
    CHECK(deliver_raw(frame, len, 2412, -70) == -EINVAL);
    frame[36] = WLAN_EID_SSID;
    len = raw_beacon(frame, 3, 0);
    frame[len] = 200;
    frame[len + 1] = 255;
    len += 2 + 255;
    while (len < 36 + WLAN_MAX_BSS_IES + 1) {
        frame[len] = WLAN_EID_VENDOR_SPECIFIC;
        frame[len + 1] = 0;
        len += 2;
    }
    CHECK(deliver_raw(frame, len, 2412, -70) == -EINVAL);
    CHECK(deliver_raw(frame, 36 + WLAN_MAX_BSS_IES, 2412, -70) == 0);
    CHECK(table.count == 3);
    kheap_host_largest = 1;
    len = raw_beacon(frame, 4, 0);
    CHECK(deliver_raw(frame, len, 2412, -70) == -ENOMEM);
    kheap_host_largest = 0;
    CHECK(table.count == 3);

    cursor = n = 0;
    while (wlan_bss_next(&table, &cursor)) {
        n++;
    }
    CHECK(n == 3 && cursor == WLAN_MAX_BSS && !wlan_bss_next(&table, &cursor));
    generation = table.generation;
    CHECK(deliver_raw(frame, len, 2412, -70) == 0 && table.generation != generation);
    generation = table.generation;
    CHECK(deliver_raw(frame, len, 2412, -50) == 0 && table.generation == generation);
}

/** Entries age out after WLAN_BSS_EXPIRE_MS unless held; a full table gives
 * up its oldest entry nobody holds, and refuses when all are held. */
static void bss_aging(void)
{
    struct wlan_scan_request r;
    struct wlan_bss *held, *bss;
    uint8_t frame[256];
    unsigned len, i;

    setup(&soft_ops);
    len = raw_beacon(frame, 1, 1);
    CHECK(deliver_raw(frame, len, 2412, -60) == 0);
    held = wlan_bss_find(&table, frame + 16, 0);
    CHECK(held != 0);
    wlan_bss_hold(held);
    len = raw_beacon(frame, 2, 1);
    CHECK(deliver_raw(frame, len, 2412, -60) == 0);
    now += WLAN_BSS_EXPIRE_MS;
    wlan_bss_expire(&table, now, WLAN_BSS_EXPIRE_MS);
    CHECK(table.count == 2);
    now += 1;
    make_request(&r, 0, "");
    r.n_frequencies = 1;
    r.frequencies[0] = 2472;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(WLAN_SCAN_PASSIVE_DWELL_MS + 1);
    CHECK(count_events(WLAN_EVENT_SCAN_DONE) == 1);
    CHECK(table.count == 1 && wlan_bss_find(&table, held->bssid, 0) == held);
    wlan_bss_put(held);
    wlan_bss_put(held);
    CHECK(held->refs == 0);
    wlan_bss_expire(&table, now, WLAN_BSS_EXPIRE_MS);
    CHECK(table.count == 0);

    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        now++;
        len = raw_beacon(frame, 100 + i, 1);
        CHECK(deliver_raw(frame, len, 2412, -60) == 0);
    }
    CHECK(table.count == WLAN_MAX_BSS);
    len = raw_beacon(frame, 100, 1);
    held = wlan_bss_find(&table, frame + 16, 2412);
    CHECK(held != 0);
    wlan_bss_hold(held);
    now++;
    len = raw_beacon(frame, 500, 1);
    CHECK(deliver_raw(frame, len, 2412, -60) == 0);
    CHECK(table.count == WLAN_MAX_BSS && wlan_bss_find(&table, held->bssid, 0) == held);
    len = raw_beacon(frame, 101, 1);
    CHECK(!wlan_bss_find(&table, frame + 16, 0));
    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        if (table.slots[i]) {
            table.slots[i]->refs++;
        }
    }
    len = raw_beacon(frame, 600, 1);
    CHECK(deliver_raw(frame, len, 2412, -60) == -ENOSPC);
    len = raw_beacon(frame, 500, 1);
    CHECK(deliver_raw(frame, len, 2412, -40) == 0);
    bss = wlan_bss_find(&table, frame + 16, 0);
    CHECK(bss && bss->signal == -4000);
    wlan_bss_put(held);
    wlan_bss_flush(&table);
    CHECK(table.count == WLAN_MAX_BSS);
    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        if (table.slots[i] && table.slots[i] != held) {
            wlan_bss_put(table.slots[i]);
        }
    }
    wlan_bss_flush(&table);
    CHECK(table.count == 1 && wlan_bss_find(&table, held->bssid, 0) == held);
}

int main(void)
{
    queue_order();
    full_scan();
    directed_scan();
    passive_scan();
    abort_scan();
    refusals();
    hardware_scan();
    bss_fields();
    bss_aging();
    wlan_bss_destroy(&table);
    if (failures) {
        printf("wlan_scan_hosttest: %u failures\n", failures);
        return 1;
    }
    printf("wlan_scan_hosttest: ok\n");
    return 0;
}
