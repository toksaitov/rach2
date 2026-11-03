/* Drive the station's MLME against a scripted AP, a clock and a work queue.
 * host-deps: rach2/sys/network/wireless/mlme.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "../../rach2/sys/network/wireless/mlme.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"
#include "../../rach2/sys/timer.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            printf("wlan_mlme_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

#define HDR       IEEE80211_HDRLEN
#define MAX_SENT  16
#define MAX_EVENT 16
#define MAX_WORK  4

/* A frame the station sent, as the data path would have taken it. */
struct sent {
    uint8_t f[WLAN_MLME_FRAME_MAX];
    unsigned len, flags;
};
/* An event with a copy of its frame, which the MLME only lends. */
struct seen {
    struct wlan_event ev;
    uint8_t f[512];
    int has_frame;
};

static uint64_t now;
static struct wlan_work *works[MAX_WORK];
static struct wlan_work *running;
static struct sent sent[MAX_SENT];
static unsigned n_sent;
static struct seen seen[MAX_EVENT];
static unsigned n_seen;
static enum wlan_sta_state states[32];
static unsigned n_states;
static int refuse_state = -1; /* a state the radio refuses to enter */
static unsigned configured;   /* MHz of the last channel the radio took */
static unsigned n_polls;      /* null frames the station asked for */
static uint64_t poll_cookie;  /* the last one's */
static uint64_t heard_at;     /* what the data path says it last heard */
static uint32_t losses;       /* the station dump's beacon loss */
static int held;              /* the data path's transmit is held */
static unsigned holds;        /* calls of the interface's hold */
static unsigned moved_to;     /* MHz of the interface's last moved */
static unsigned moves;        /* calls of the interface's moved */

static struct wlan_radio radio;
static struct wlan_mlme mlme;
static struct wlan_scan scan;
static struct wlan_bss bss_a, bss_b;
static const uint8_t sta[6] = {2, 0, 0, 0, 0, 1};
static const uint8_t ap_a[6] = {2, 0, 0, 0, 0xa, 1};
static const uint8_t ap_b[6] = {2, 0, 0, 0, 0xa, 2};
static const uint8_t other[6] = {2, 0, 0, 0, 0, 9};
static const uint8_t group[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/** The model's clock: one tick per millisecond. */
uint64_t tick_count(void)
{
    return now;
}

/** Queue work in the model's list, refusing one pending or running. */
int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    unsigned i, free;

    free = MAX_WORK;
    for (i = 0; i < MAX_WORK; ++i) {
        if (works[i] == work)
            return -EBUSY;
        if (!works[i])
            free = i;
    }
    if (work == running || free == MAX_WORK)
        return -EBUSY;
    work->due = due ? due : now;
    works[free] = work;
    return 0;
}

/** Remove queued work; the model never cancels a running one. */
void wlan_cancel_work(struct wlan_work *work)
{
    unsigned i;

    CHECK(work != running);
    for (i = 0; i < MAX_WORK; ++i)
        if (works[i] == work)
            works[i] = 0;
}

/** Hold a BSS, as bss.c does. */
void wlan_bss_hold(struct wlan_bss *bss)
{
    bss->refs++;
}

/** Drop a hold, as bss.c does. */
void wlan_bss_put(struct wlan_bss *bss)
{
    CHECK(bss->refs != 0);
    if (bss->refs)
        bss->refs--;
}

/** Keep the scan's home channel, which the MLME sets. */
void wlan_scan_set_home(struct wlan_scan *s, const struct wlan_channel *channel, unsigned filters)
{
    CHECK(filters == 0);
    s->home = channel;
}

/** Keep the event and a copy of its borrowed frame. */
static void iface_event(struct wlan *wlan, const struct wlan_event *event)
{
    struct seen *s;

    (void)wlan;
    if (n_seen == MAX_EVENT)
        return;
    s = &seen[n_seen++];
    s->ev = *event;
    s->has_frame = event->frame != 0;
    if (event->frame && event->frame_len <= sizeof(s->f))
        k_memcpy(s->f, event->frame, event->frame_len);
    s->ev.frame = 0;
}

/** Run the queued work due by time t in the order of its deadlines. */
static void run_until(uint64_t t)
{
    struct wlan_work *w;
    unsigned i, best;

    for (;;) {
        best = MAX_WORK;
        for (i = 0; i < MAX_WORK; ++i)
            if (works[i] && works[i]->due <= t &&
                (best == MAX_WORK || works[i]->due < works[best]->due))
                best = i;
        if (best == MAX_WORK)
            break;
        w = works[best];
        works[best] = 0;
        if (w->due > now)
            now = w->due;
        running = w;
        w->run(w);
        running = 0;
    }
    now = t;
}

/** Advance the clock by ms. */
static void wait_ms(unsigned ms)
{
    run_until(now + ms);
}

/** The number of work records queued. */
static unsigned queued(void)
{
    unsigned i, n;

    for (i = n = 0; i < MAX_WORK; ++i)
        n += works[i] != 0;
    return n;
}

static int radio_start(struct wlan_radio *r)
{
    (void)r;
    return 0;
}

static void radio_stop(struct wlan_radio *r)
{
    (void)r;
}

static int radio_config(struct wlan_radio *r, const struct wlan_channel *c, unsigned filters)
{
    (void)r;
    CHECK(filters == 0);
    configured = c->frequency;
    return 0;
}

static int radio_tx(struct wlan_radio *r, const uint8_t *f, unsigned len,
                    const struct wlan_tx_info *info)
{
    (void)r;
    (void)f;
    (void)len;
    (void)info;
    printf("wlan_mlme_hosttest: the MLME sent past its interface\n");
    ++failures;
    return -EIO;
}

static const struct wlan_radio_ops radio_ops = {
    radio_start, radio_stop, radio_config, radio_tx, 0, 0, 0, 0, 0, 0};

/** Keep a copy of the frame the station sends. */
static int iface_tx(struct wlan *wlan, const uint8_t *f, unsigned len, unsigned flags)
{
    (void)wlan;
    CHECK(len >= HDR && len <= WLAN_MLME_FRAME_MAX);
    if (n_sent < MAX_SENT && len <= WLAN_MLME_FRAME_MAX) {
        k_memcpy(sent[n_sent].f, f, len);
        sent[n_sent].len = len;
        sent[n_sent].flags = flags;
        ++n_sent;
    }
    return 0;
}

/** Commit a state as wlan_set_state does: refuse refuse_state, and
 * record the rest with a BSS but for NONE. */
static int iface_state(struct wlan *wlan, enum wlan_sta_state state, struct wlan_bss *bss)
{
    (void)wlan;
    CHECK((state == WLAN_STA_NONE) == (bss == 0));
    if ((int)state == refuse_state)
        return -EIO;
    if (n_states < sizeof(states) / sizeof(states[0]))
        states[n_states++] = state;
    return 0;
}

/** Hold or release the data path's transmit, as the softMAC station does;
 * a hold is released before another comes. */
static void iface_hold(struct wlan *wlan, int h)
{
    (void)wlan;
    CHECK(held != h);
    held = h;
    ++holds;
}

/** Note the channel the station followed its AP to, which the MLME has
 * tuned the radio to and given the BSS first. */
static void iface_moved(struct wlan *wlan, const struct wlan_channel *channel)
{
    (void)wlan;
    CHECK(configured == channel->frequency && mlme.bss->frequency == channel->frequency);
    moved_to = channel->frequency;
    ++moves;
}

/* An interface that learns of no ACK, which probes a silent AP. */
static const struct wlan_mlme_ops iface_ops = {iface_tx, iface_state, iface_event, 0,
                                               0,        iface_hold,  iface_moved};

/** Note a null frame the station asks the data path to send. */
static int iface_poll(struct wlan *wlan, uint64_t *cookie)
{
    (void)wlan;
    ++n_polls;
    *cookie = ++poll_cookie;
    return 0;
}

/** When the data path last heard from the AP. */
static uint64_t iface_heard(struct wlan *wlan)
{
    (void)wlan;
    return heard_at;
}

/* An interface that reports ACKs, as the softMAC station over ath9k does. */
static const struct wlan_mlme_ops poll_ops = {iface_tx,    iface_state, iface_event, iface_poll,
                                              iface_heard, iface_hold,  iface_moved};

/** Compare n bytes. */
static int equal(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static unsigned get16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/** Write a management header from sa in bssid to da. */
static void hdr(uint8_t *f, unsigned stype, const uint8_t *da, const uint8_t *sa,
                const uint8_t *bssid)
{
    k_memset(f, 0, HDR);
    put16(f, IEEE80211_FTYPE_MGMT | stype);
    k_memcpy(f + 4, da, 6);
    k_memcpy(f + 10, sa, 6);
    k_memcpy(f + 16, bssid, 6);
}

/** Append element id with len bytes of p at *pos. */
static void elem(uint8_t *f, unsigned *pos, unsigned id, const uint8_t *p, unsigned len)
{
    f[*pos] = (uint8_t)id;
    f[*pos + 1] = (uint8_t)len;
    k_memcpy(f + *pos + 2, p, len);
    *pos += len + 2;
}

/* The AP's elements: its rates, HT and WMM parameters. */
static const uint8_t ap_rates[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
static const uint8_t ap_ext_rates[4] = {0x30, 0x48, 0x60, 0x6c};
static const uint8_t ap_wmm[24] = {0,    0x50, 0xf2, 2, 1,    1,    0,    0, 3,    0xa4, 0,    0,
                                   0x27, 0xa4, 0,    0, 0x42, 0x43, 0x5e, 0, 0x62, 0x32, 0x2f, 0};

/** Append the AP's rates, HT capabilities and operation and WMM. */
static void ap_elems(uint8_t *f, unsigned *pos)
{
    uint8_t ht[IEEE80211_HT_CAP_LEN], op[IEEE80211_HT_OPERATION_LEN];

    k_memset(ht, 0, sizeof(ht));
    k_memset(op, 0, sizeof(op));
    ht[0] = 0x2c;
    ht[3] = 0xff;
    op[0] = 1;
    elem(f, pos, WLAN_EID_SUPP_RATES, ap_rates, sizeof(ap_rates));
    elem(f, pos, WLAN_EID_EXT_SUPP_RATES, ap_ext_rates, sizeof(ap_ext_rates));
    elem(f, pos, WLAN_EID_HT_CAPABILITY, ht, sizeof(ht));
    elem(f, pos, WLAN_EID_HT_OPERATION, op, sizeof(op));
    elem(f, pos, WLAN_EID_VENDOR_SPECIFIC, ap_wmm, sizeof(ap_wmm));
}

/** Fill a BSS of the network "lab" as the BSS table would. */
static void make_bss(struct wlan_bss *b, const uint8_t *bssid, unsigned frequency)
{
    unsigned pos;

    k_memset(b, 0, sizeof(*b));
    k_memcpy(b->bssid, bssid, 6);
    k_memcpy(b->ssid, "lab", 3);
    b->ssid_len = 3;
    b->frequency = (uint16_t)frequency;
    b->beacon_interval = 100;
    b->capability = WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY;
    pos = 0;
    elem(b->ies, &pos, WLAN_EID_SSID, b->ssid, 3);
    ap_elems(b->ies, &pos);
    b->ies_len = pos;
}

/** Give the radio channels 1 to 13, the 802.11g rates and HT. */
static void make_radio(void)
{
    static const uint16_t rates[12] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};
    unsigned i;

    k_memset(&radio, 0, sizeof(radio));
    radio.ops = &radio_ops;
    radio.name = "stub";
    k_memcpy(radio.mac, sta, 6);
    for (i = 0; i < 13; ++i) {
        radio.band.channels[i].frequency = (uint16_t)(2412 + 5 * i);
        radio.band.channels[i].number = (uint8_t)(i + 1);
    }
    radio.band.n_channels = 13;
    k_memcpy(radio.band.rates, rates, sizeof(rates));
    radio.band.n_rates = 12;
    radio.band.ht_supported = 1;
    radio.band.ht_cap = 0x116e;
    radio.band.ampdu_factor = 3;
    radio.band.ampdu_density = 6;
    radio.band.mcs[0] = 0xff;
}

/** Start a case from nothing: a new MLME, two BSSes, empty records. */
static void reset(void)
{
    k_memset(works, 0, sizeof(works));
    n_sent = n_seen = n_states = 0;
    refuse_state = -1;
    configured = 0;
    held = 0;
    holds = moved_to = moves = 0;
    now = 1000;
    make_radio();
    make_bss(&bss_a, ap_a, 2412);
    make_bss(&bss_b, ap_b, 2437);
    k_memset(&scan, 0, sizeof(scan));
    wlan_mlme_init(&mlme, 0, &radio, &radio.band, &scan, &iface_ops);
}

/** Give the station a frame from the AP, unprotected unless flags say. */
static int deliver(const uint8_t *f, unsigned len, unsigned flags)
{
    return wlan_mlme_rx(&mlme, f, len, flags);
}

/** The AP's authentication frame of algorithm, transaction and status,
 * with n bytes of body. */
static int ap_auth(const uint8_t *ap, unsigned algorithm, unsigned trans, unsigned status,
                   unsigned n)
{
    uint8_t f[HDR + 6 + 64];
    unsigned i;

    hdr(f, IEEE80211_STYPE_AUTH, sta, ap, ap);
    put16(f + HDR, algorithm);
    put16(f + HDR + 2, trans);
    put16(f + HDR + 4, status);
    for (i = 0; i < n && i < 64; ++i)
        f[HDR + 6 + i] = (uint8_t)(0x40 + i);
    return deliver(f, HDR + 6 + n, 0);
}

/** The AP's association response with status, AID and its elements; a
 * nonzero comeback adds a Timeout Interval of that many TUs. */
static int ap_assoc(const uint8_t *ap, unsigned stype, unsigned status, unsigned comeback)
{
    uint8_t f[256], t[5];
    unsigned pos;

    hdr(f, stype, sta, ap, ap);
    put16(f + HDR, WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY);
    put16(f + HDR + 2, status);
    put16(f + HDR + 4, 0xc001);
    pos = HDR + 6;
    ap_elems(f, &pos);
    if (comeback) {
        t[0] = 3;
        t[1] = (uint8_t)comeback;
        t[2] = (uint8_t)(comeback >> 8);
        t[3] = t[4] = 0;
        elem(f, &pos, WLAN_EID_TIMEOUT_INTERVAL, t, sizeof(t));
    }
    return deliver(f, pos, 0);
}

/** A beacon or probe response of ap. */
static int ap_beacon(const uint8_t *ap, unsigned stype)
{
    uint8_t f[HDR + 12 + 5];
    unsigned pos;

    hdr(f, stype, stype == IEEE80211_STYPE_BEACON ? group : sta, ap, ap);
    k_memset(f + HDR, 0, 12);
    f[HDR + 8] = 100;
    pos = HDR + 12;
    elem(f, &pos, WLAN_EID_SSID, (const uint8_t *)"lab", 3);
    return deliver(f, pos, 0);
}

/** A beacon of ap that carries element id with the n bytes of p, or
 * none when n is zero. */
static int ap_beacon_with(const uint8_t *ap, unsigned id, const uint8_t *p, unsigned n)
{
    uint8_t f[HDR + 12 + 5 + 2 + 8];
    unsigned pos;

    hdr(f, IEEE80211_STYPE_BEACON, group, ap, ap);
    k_memset(f + HDR, 0, 12);
    f[HDR + 8] = 100;
    pos = HDR + 12;
    elem(f, &pos, WLAN_EID_SSID, (const uint8_t *)"lab", 3);
    if (n)
        elem(f, &pos, id, p, n);
    return deliver(f, pos, 0);
}

/** A deauthentication or disassociation of reason from ap to da. */
static int ap_leave(const uint8_t *ap, const uint8_t *da, unsigned stype, unsigned reason,
                    unsigned flags)
{
    uint8_t f[HDR + 2];

    hdr(f, stype, da, ap, ap);
    put16(f + HDR, reason);
    return deliver(f, sizeof(f), flags);
}

/** An action frame of category and action from ap, with a two-byte
 * transaction identifier. */
static int ap_action(const uint8_t *ap, unsigned category, unsigned action, unsigned flags)
{
    uint8_t f[HDR + 4];

    hdr(f, IEEE80211_STYPE_ACTION, sta, ap, ap);
    f[HDR] = (uint8_t)category;
    f[HDR + 1] = (uint8_t)action;
    f[HDR + 2] = 0x5a;
    f[HDR + 3] = 0xa5;
    return deliver(f, sizeof(f), flags);
}

/** Authenticate with Open System to b. */
static int open_auth(struct wlan_bss *b)
{
    struct wlan_mlme_auth req;

    k_memset(&req, 0, sizeof(req));
    req.bss = b;
    req.algorithm = WLAN_AUTH_OPEN;
    return wlan_mlme_authenticate(&mlme, &req);
}

/* The supplicant's elements: RSN with CCMP and SAE, MFPC, then extended
 * capabilities, RSNX and supported operating classes. */
static const uint8_t user_ies[] = {48,   20, 1,    0,   0, 0x0f, 0xac, 4, 1,    0,    0,   0x0f,
                                   0xac, 4,  1,    0,   0, 0x0f, 0xac, 8, 0x80, 0,    127, 3,
                                   0,    0,  0x08, 244, 1, 0x20, 59,   3, 0x51, 0x51, 0x53};

/** Associate with b, reassociating from prev when it is not NULL. */
static int assoc(struct wlan_bss *b, const uint8_t *prev, int mfp)
{
    struct wlan_mlme_assoc req;

    k_memset(&req, 0, sizeof(req));
    req.bss = b;
    req.ies = user_ies;
    req.ies_len = sizeof(user_ies);
    req.prev_bssid = prev;
    req.mfp = mfp;
    return wlan_mlme_associate(&mlme, &req);
}

/** The last event, or a blank one when there is none. */
static const struct seen *last_event(void)
{
    static struct seen none;

    return n_seen ? &seen[n_seen - 1] : &none;
}

/** Whether sent frame s is of subtype stype from the station to da. */
static int sent_is(const struct sent *s, unsigned stype, const uint8_t *da)
{
    return get16(s->f) == (IEEE80211_FTYPE_MGMT | stype) && equal(s->f + 4, da, 6) &&
           equal(s->f + 10, sta, 6) && equal(s->f + 16, da, 6);
}

/** Bring the station to ASSOCIATED with ap_a through Open System. */
static void connect_a(int mfp)
{
    CHECK(open_auth(&bss_a) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(assoc(&bss_a, 0, mfp) == 0);
    CHECK(ap_assoc(ap_a, IEEE80211_STYPE_ASSOC_RESP, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(mlme.state == WLAN_STA_ASSOCIATED);
    n_sent = n_seen = n_states = 0;
}

/** Check the association request's fixed fields and its elements in
 * order: SSID, split rates, RSN, HT for 20 MHz, extended capabilities, WMM,
 * RSNX, then the rest of the supplicant's. */
static void check_assoc_request(const struct sent *s)
{
    static const uint8_t rates[] = {1, 8, 2, 4, 11, 22, 12, 18, 24, 36, 50, 4, 48, 72, 96, 108};
    static const uint8_t wmm[] = {221, 7, 0, 0x50, 0xf2, 2, 0, 1, 0};
    const uint8_t *p;

    CHECK(sent_is(s, IEEE80211_STYPE_ASSOC_REQ, ap_a));
    CHECK(get16(s->f + HDR) == (WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY |
                                WLAN_CAPABILITY_SHORT_PREAMBLE | WLAN_CAPABILITY_SHORT_SLOT_TIME));
    CHECK(get16(s->f + HDR + 2) == WLAN_MLME_LISTEN_INTERVAL);
    p = s->f + HDR + 4;
    CHECK(p[0] == WLAN_EID_SSID && p[1] == 3 && equal(p + 2, (const uint8_t *)"lab", 3));
    p += 5;
    CHECK(equal(p, rates, sizeof(rates)));
    p += sizeof(rates);
    CHECK(equal(p, user_ies, 22));
    p += 22;
    CHECK(p[0] == WLAN_EID_HT_CAPABILITY && p[1] == 26);
    CHECK(get16(p + 2) == 0x112c && p[4] == (3 | 6 << 2) && p[5] == 0xff);
    p += 28;
    CHECK(equal(p, user_ies + 22, 5));
    p += 5;
    CHECK(equal(p, wmm, sizeof(wmm)));
    p += sizeof(wmm);
    CHECK(equal(p, user_ies + 27, 8));
    p += 8;
    CHECK(p == s->f + s->len);
}

/** Open System, association with every element, the response taken into
 * the station's state, and the control port opened and closed. */
static void open_association(void)
{
    reset();
    CHECK(open_auth(&bss_a) == 0);
    CHECK(configured == 2412 && bss_a.refs == 1);
    CHECK(scan.home == mlme.channel && mlme.channel->frequency == 2412);
    CHECK(mlme.state == WLAN_STA_AUTHENTICATING && n_sent == 1);
    CHECK(sent_is(&sent[0], IEEE80211_STYPE_AUTH, ap_a) && sent[0].len == HDR + 6);
    CHECK(get16(sent[0].f + HDR) == 0 && get16(sent[0].f + HDR + 2) == 1);
    CHECK(get16(sent[0].f + HDR + 4) == 0 && sent[0].flags == 0);
    CHECK(ap_auth(ap_b, WLAN_AUTH_OPEN, 2, 0, 0) == 1 && n_seen == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(mlme.state == WLAN_STA_AUTHENTICATED && n_seen == 1);
    CHECK(seen[0].ev.type == WLAN_EVENT_AUTHENTICATE && seen[0].has_frame);
    CHECK(seen[0].ev.status == 0 && equal(seen[0].ev.peer, ap_a, 6));
    CHECK(seen[0].ev.frequency == 2412 && seen[0].ev.frame_len == HDR + 6);
    CHECK(assoc(&bss_a, 0, 1) == 0 && mlme.state == WLAN_STA_ASSOCIATING && n_sent == 2);
    check_assoc_request(&sent[1]);
    CHECK(ap_assoc(ap_a, IEEE80211_STYPE_ASSOC_RESP, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(mlme.state == WLAN_STA_ASSOCIATED && n_seen == 2);
    CHECK(seen[1].ev.type == WLAN_EVENT_ASSOCIATE && seen[1].has_frame);
    CHECK(seen[1].ev.status == 0 && seen[1].ev.frame_len > HDR + 6);
    CHECK(mlme.aid == 1 && mlme.qos && mlme.ht && mlme.mfp && mlme.n_rates == 12);
    CHECK(mlme.capability == (WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY));
    CHECK(wlan_mlme_authorize(&mlme, 1) == 0 && mlme.state == WLAN_STA_AUTHORIZED);
    CHECK(wlan_mlme_authorize(&mlme, 0) == 0 && mlme.state == WLAN_STA_ASSOCIATED);
    CHECK(wlan_mlme_authorize(&mlme, 1) == 0);
    CHECK(n_states == 7 && states[0] == WLAN_STA_AUTHENTICATING);
    CHECK(states[3] == WLAN_STA_ASSOCIATED && states[6] == WLAN_STA_AUTHORIZED);
    CHECK(mlme.frame_len == sent[1].len && equal(mlme.frame, sent[1].f, sent[1].len));
    CHECK(queued() == 1);
}

/** Fill total bytes of ies with vendor elements of an unknown OUI. */
static void vendor_fill(uint8_t *ies, unsigned total)
{
    unsigned n;

    k_memset(ies, 0, total);
    while (total) {
        n = total > 257 ? 257 : total;
        if (total - n == 1)
            --n;
        ies[0] = WLAN_EID_VENDOR_SPECIFIC;
        ies[1] = (uint8_t)(n - 2);
        ies += n;
        total -= n;
    }
}

/** Requests the MLME refuses, each leaving the state as it was, and the
 * largest request that fits. */
static void refusals(void)
{
    struct wlan_mlme_auth req;
    static uint8_t big[WLAN_MLME_FRAME_MAX];
    static const uint8_t sae[2] = {1, 0};
    static const uint8_t repeat[] = {0, 0, 0, 0};

    reset();
    k_memset(&req, 0, sizeof(req));
    req.bss = &bss_a;
    req.algorithm = 1;
    CHECK(wlan_mlme_authenticate(&mlme, &req) == -EOPNOTSUPP);
    req.algorithm = WLAN_AUTH_SAE;
    req.data = sae;
    req.data_len = sizeof(sae);
    CHECK(wlan_mlme_authenticate(&mlme, &req) == -EINVAL);
    req.algorithm = WLAN_AUTH_OPEN;
    req.data_len = 0;
    req.ies = repeat;
    req.ies_len = sizeof(repeat);
    CHECK(wlan_mlme_authenticate(&mlme, &req) == -EINVAL);
    req.ies = big;
    req.ies_len = WLAN_MLME_FRAME_MAX - HDR - 6 + 4;
    vendor_fill(big, req.ies_len);
    CHECK(wlan_mlme_authenticate(&mlme, &req) == -EMSGSIZE);
    radio.band.channels[12].flags = WLAN_CHANNEL_DISABLED;
    make_bss(&bss_b, ap_b, 2472);
    CHECK(open_auth(&bss_b) == -EINVAL && bss_b.refs == 0);
    CHECK(assoc(&bss_a, 0, 0) == -ENOLINK);
    CHECK(wlan_mlme_deauthenticate(&mlme, ap_a, 3, 0) == -ENOTCONN);
    CHECK(wlan_mlme_disassociate(&mlme, ap_a, 3, 0) == -ENOTCONN);
    CHECK(wlan_mlme_authorize(&mlme, 1) == -ENOTCONN);
    CHECK(n_sent == 0 && n_states == 0 && bss_a.refs == 0);
    req.ies_len = WLAN_MLME_FRAME_MAX - HDR - 6;
    vendor_fill(big, req.ies_len);
    CHECK(wlan_mlme_authenticate(&mlme, &req) == 0);
    CHECK(n_sent == 1 && sent[0].len == WLAN_MLME_FRAME_MAX);
    CHECK(equal(sent[0].f + HDR + 6, big, req.ies_len));
    CHECK(assoc(&bss_a, 0, 0) == -ENOLINK);
    CHECK(wlan_mlme_disassociate(&mlme, ap_a, 3, 0) == -ENOTCONN);
    CHECK(wlan_mlme_deauthenticate(&mlme, ap_b, 3, 0) == -ENOTCONN);
    CHECK(n_sent == 1 && n_seen == 0 && mlme.state == WLAN_STA_AUTHENTICATING);
}

/** Open System unanswered: three tries 200 ms apart, then a timeout
 * reported with no frame; a refusal reported with the AP's frame. */
static void open_timeout(void)
{
    reset();
    CHECK(open_auth(&bss_a) == 0);
    wait_ms(199);
    CHECK(n_sent == 1);
    wait_ms(1);
    CHECK(n_sent == 2 && equal(sent[1].f, sent[0].f, sent[0].len));
    wait_ms(200);
    CHECK(n_sent == 3 && n_seen == 0);
    wait_ms(200);
    CHECK(n_sent == 3 && n_seen == 1 && mlme.state == WLAN_STA_NONE);
    CHECK(seen[0].ev.type == WLAN_EVENT_AUTHENTICATE && !seen[0].has_frame);
    CHECK(equal(seen[0].ev.peer, ap_a, 6) && bss_a.refs == 0 && queued() == 0);
    CHECK(open_auth(&bss_a) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG, 0) == 1);
    CHECK(mlme.state == WLAN_STA_NONE && n_seen == 2 && seen[1].has_frame);
    CHECK(seen[1].ev.status == WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG && queued() == 0);
    CHECK(open_auth(&bss_a) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    wait_ms(5000);
    CHECK(mlme.state == WLAN_STA_NONE && n_seen == 3 && bss_a.refs == 0);
}

/** Association refused, refused for now with a comeback time, unanswered,
 * and refused by the radio. */
static void association_failures(void)
{
    reset();
    CHECK(open_auth(&bss_a) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(assoc(&bss_a, 0, 0) == 0);
    CHECK(ap_assoc(ap_a, IEEE80211_STYPE_ASSOC_RESP, WLAN_STATUS_AP_UNABLE_TO_HANDLE_NEW_STA, 0));
    CHECK(mlme.state == WLAN_STA_NONE && last_event()->has_frame);
    CHECK(last_event()->ev.type == WLAN_EVENT_ASSOCIATE);
    CHECK(last_event()->ev.status == WLAN_STATUS_AP_UNABLE_TO_HANDLE_NEW_STA);
    CHECK(open_auth(&bss_a) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(assoc(&bss_a, 0, 1) == 0);
    n_sent = n_seen = 0;
    CHECK(ap_assoc(ap_a, IEEE80211_STYPE_ASSOC_RESP, WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY, 293));
    CHECK(mlme.state == WLAN_STA_ASSOCIATING && n_seen == 0);
    wait_ms(299);
    CHECK(n_sent == 0);
    wait_ms(1);
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_ASSOC_REQ, ap_a));
    wait_ms(200);
    CHECK(n_sent == 2 && n_seen == 0);
    wait_ms(200);
    CHECK(mlme.state == WLAN_STA_NONE && n_seen == 1 && !seen[0].has_frame);
    CHECK(seen[0].ev.type == WLAN_EVENT_ASSOCIATE && bss_a.refs == 0);
    CHECK(open_auth(&bss_a) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(assoc(&bss_a, 0, 0) == 0);
    refuse_state = WLAN_STA_ASSOCIATED;
    CHECK(ap_assoc(ap_a, IEEE80211_STYPE_ASSOC_RESP, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(mlme.state == WLAN_STA_NONE && !last_event()->has_frame);
    CHECK(last_event()->ev.type == WLAN_EVENT_ASSOCIATE && queued() == 0);
}

/** Roam from ap_a to ap_b of the same network: leaving ap_a is reported
 * but not sent, and the request to ap_b is a reassociation from ap_a. */
static void reassociation(void)
{
    reset();
    connect_a(0);
    CHECK(open_auth(&bss_b) == 0);
    CHECK(n_seen == 1 && seen[0].ev.type == WLAN_EVENT_DEAUTHENTICATE);
    CHECK(seen[0].ev.locally_generated && seen[0].ev.reason == WLAN_REASON_UNSPECIFIED);
    CHECK(equal(seen[0].ev.peer, ap_a, 6) && seen[0].has_frame);
    CHECK(get16(seen[0].f) == (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_DEAUTH));
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_AUTH, ap_b));
    CHECK(bss_a.refs == 0 && bss_b.refs == 1 && configured == 2437);
    CHECK(scan.home && scan.home->frequency == 2437);
    CHECK(ap_auth(ap_b, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(assoc(&bss_b, ap_a, 0) == 0);
    CHECK(sent_is(&sent[1], IEEE80211_STYPE_REASSOC_REQ, ap_b));
    CHECK(equal(sent[1].f + HDR + 4, ap_a, 6));
    CHECK(sent[1].f[HDR + 10] == WLAN_EID_SSID);
    CHECK(ap_assoc(ap_b, IEEE80211_STYPE_REASSOC_RESP, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(mlme.state == WLAN_STA_ASSOCIATED && last_event()->ev.type == WLAN_EVENT_ASSOCIATE);
    CHECK(equal(last_event()->ev.peer, ap_b, 6) && last_event()->ev.frequency == 2437);
}

/** An AP that vanishes: beacons keep the station, their loss starts
 * probes, an answer ends them, and three unanswered probes end it. */
static void vanishing_ap(void)
{
    unsigned i;

    reset();
    connect_a(0);
    for (i = 0; i < 20; ++i) {
        wait_ms(102);
        CHECK(ap_beacon(ap_a, IEEE80211_STYPE_BEACON) == 0);
    }
    CHECK(n_sent == 0 && queued() == 1);
    wait_ms(715);
    CHECK(n_sent == 0);
    wait_ms(1);
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_PROBE_REQ, ap_a));
    CHECK(sent[0].f[HDR] == WLAN_EID_SSID && sent[0].f[HDR + 1] == 3);
    CHECK(mlme.missed_beacons == 1);
    CHECK(ap_beacon(ap_a, IEEE80211_STYPE_PROBE_RESP) == 0 && mlme.probes == 0);
    wait_ms(715);
    CHECK(n_sent == 1);
    wait_ms(1);
    CHECK(n_sent == 2);
    wait_ms(500);
    CHECK(n_sent == 3);
    wait_ms(500);
    CHECK(n_sent == 4 && n_seen == 0);
    wait_ms(499);
    CHECK(mlme.state == WLAN_STA_ASSOCIATED);
    wait_ms(1);
    CHECK(n_sent == 4 && mlme.state == WLAN_STA_NONE && n_seen == 1);
    CHECK(seen[0].ev.type == WLAN_EVENT_DEAUTHENTICATE && seen[0].ev.locally_generated);
    CHECK(seen[0].ev.reason == WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY && seen[0].has_frame);
    CHECK(bss_a.refs == 0 && queued() == 0 && states[n_states - 1] == WLAN_STA_NONE);
    CHECK(!scan.home && !mlme.channel);
}

/** Wait ms milliseconds in beacon intervals of 100 ms, the AP's beacon
 * heard at the end of each. */
static void beacons_for(unsigned ms)
{
    for (; ms >= 100; ms -= 100) {
        wait_ms(100);
        CHECK(ap_beacon(ap_a, IEEE80211_STYPE_BEACON) == 0);
    }
}

/** An interface that reports ACKs: the station polls the AP with a null
 * frame thirty seconds after it last heard from it, and seven beacon
 * intervals after its last beacon, as mac80211 does on ath9k; an ACK or a
 * beacon ends the poll, a frame not acknowledged is asked again at once,
 * and two of them, or two whose status never came, end the association. */
static void polled_ap(void)
{
    uint64_t t;

    reset();
    wlan_mlme_init(&mlme, 0, &radio, &radio.band, &scan, &poll_ops);
    mlme.beacon_losses = &losses;
    losses = 0;
    n_polls = 0;
    heard_at = 0;
    connect_a(0);
    t = now;

    /* Idle: the AP's beacons alone do not count as hearing from it. */
    beacons_for(29900);
    CHECK(n_polls == 0 && n_sent == 0);
    wait_ms(100);
    CHECK(n_polls == 1 && n_sent == 0 && now == t + WLAN_MLME_IDLE_POLL);
    CHECK(mlme.probes == 1 && mlme.missed_beacons == 0);
    wlan_mlme_tx_status(&mlme, poll_cookie + 1, 1);
    CHECK(mlme.probes == 1);
    wlan_mlme_tx_status(&mlme, poll_cookie, 1);
    CHECK(mlme.probes == 0 && mlme.poll_cookie == 0);

    /* A unicast frame or an ACK the data path heard moves the next poll. */
    beacons_for(20000);
    heard_at = now;
    beacons_for(29900);
    CHECK(n_polls == 1);
    wait_ms(100);
    CHECK(n_polls == 2);

    /* Not acknowledged: asked again at once; a beacon ends the poll. */
    wlan_mlme_tx_status(&mlme, poll_cookie, 0);
    CHECK(n_polls == 3 && mlme.probes == 2);
    CHECK(ap_beacon(ap_a, IEEE80211_STYPE_BEACON) == 0 && mlme.probes == 0);
    wlan_mlme_tx_status(&mlme, poll_cookie, 0);
    CHECK(n_polls == 3 && mlme.state == WLAN_STA_ASSOCIATED);

    /* Seven intervals without a beacon: a poll, and one beacon loss. */
    wait_ms(715);
    CHECK(n_polls == 3);
    wait_ms(1);
    CHECK(n_polls == 4 && mlme.missed_beacons == 1 && losses == 1);
    wlan_mlme_tx_status(&mlme, poll_cookie, 1);
    CHECK(mlme.probes == 0);
    wait_ms(715);
    CHECK(n_polls == 4);
    wait_ms(1);
    CHECK(n_polls == 5 && mlme.missed_beacons == 2 && losses == 2);

    /* Two null frames not acknowledged: the station leaves, sending
     * nothing. */
    wlan_mlme_tx_status(&mlme, poll_cookie, 0);
    CHECK(n_polls == 6 && mlme.state == WLAN_STA_ASSOCIATED);
    wlan_mlme_tx_status(&mlme, poll_cookie, 0);
    CHECK(n_polls == 6 && mlme.state == WLAN_STA_NONE && n_sent == 0);
    CHECK(n_seen == 1 && seen[0].ev.type == WLAN_EVENT_DEAUTHENTICATE);
    CHECK(seen[0].ev.locally_generated);
    CHECK(seen[0].ev.reason == WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY);
    CHECK(bss_a.refs == 0 && queued() == 0);

    /* Two polls whose status never comes end it too, 500 ms each. */
    connect_a(0);
    CHECK(mlme.missed_beacons == 0 && mlme.probes == 0);
    wait_ms(716);
    CHECK(n_polls == 7 && mlme.missed_beacons == 1);
    wait_ms(WLAN_MLME_PROBE_TIMEOUT);
    CHECK(n_polls == 8 && mlme.state == WLAN_STA_ASSOCIATED);
    wait_ms(WLAN_MLME_PROBE_TIMEOUT - 1);
    CHECK(mlme.state == WLAN_STA_ASSOCIATED);
    wait_ms(1);
    CHECK(n_polls == 8 && mlme.state == WLAN_STA_NONE && n_sent == 0);
    CHECK(n_seen == 1 && seen[0].ev.type == WLAN_EVENT_DEAUTHENTICATE);
    CHECK(seen[0].ev.reason == WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY);
}

/** Under 802.11w: unprotected deauthentication and disassociation dropped
 * and reported, an SA Query answered only when protected, and a protected
 * deauthentication taken. */
static void protected_frames(void)
{
    reset();
    connect_a(1);
    CHECK(wlan_mlme_authorize(&mlme, 1) == 0);
    CHECK(ap_leave(ap_a, sta, IEEE80211_STYPE_DEAUTH, 7, 0) == 1);
    CHECK(mlme.state == WLAN_STA_AUTHORIZED && n_seen == 1 && seen[0].has_frame);
    CHECK(seen[0].ev.type == WLAN_EVENT_UNPROT_DEAUTHENTICATE && seen[0].ev.reason == 7);
    CHECK(ap_leave(ap_a, group, IEEE80211_STYPE_DISASSOC, 8, 0) == 1);
    CHECK(n_seen == 2 && seen[1].ev.type == WLAN_EVENT_UNPROT_DISASSOCIATE);
    CHECK(ap_leave(other, sta, IEEE80211_STYPE_DEAUTH, 7, 0) == 1 && n_seen == 2);
    CHECK(ap_action(ap_a, WLAN_CATEGORY_SA_QUERY, WLAN_ACTION_SA_QUERY_REQUEST, 0) == 1);
    CHECK(n_sent == 0 && n_seen == 2);
    CHECK(ap_action(ap_a, WLAN_CATEGORY_SA_QUERY, WLAN_ACTION_SA_QUERY_REQUEST,
                    WLAN_MLME_RX_PROTECTED) == 1);
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_ACTION, ap_a));
    CHECK(sent[0].len == HDR + 4 && sent[0].flags == WLAN_MLME_TX_PROTECT);
    CHECK(sent[0].f[HDR] == WLAN_CATEGORY_SA_QUERY);
    CHECK(sent[0].f[HDR + 1] == WLAN_ACTION_SA_QUERY_RESPONSE);
    CHECK(sent[0].f[HDR + 2] == 0x5a && sent[0].f[HDR + 3] == 0xa5);
    CHECK(ap_action(ap_a, WLAN_CATEGORY_SA_QUERY, WLAN_ACTION_SA_QUERY_RESPONSE,
                    WLAN_MLME_RX_PROTECTED) == 0);
    CHECK(ap_action(ap_a, WLAN_CATEGORY_BACK, 0, 0) == 1);
    CHECK(ap_action(ap_a, WLAN_CATEGORY_BACK, 0, WLAN_MLME_RX_PROTECTED) == 0);
    CHECK(ap_action(ap_a, 4, 0, 0) == 0 && n_sent == 1);
    CHECK(ap_leave(ap_a, sta, IEEE80211_STYPE_DEAUTH, 6, WLAN_MLME_RX_PROTECTED) == 1);
    CHECK(mlme.state == WLAN_STA_NONE && n_seen == 3 && !seen[2].ev.locally_generated);
    CHECK(seen[2].ev.type == WLAN_EVENT_DEAUTHENTICATE && seen[2].ev.reason == 6);
    CHECK(bss_a.refs == 0 && queued() == 0);
}

/** Without 802.11w the AP's deauthentication and disassociation are taken
 * as they come; the station's own are sent, protected under 802.11w. */
static void plain_frames(void)
{
    uint8_t f[HDR];

    reset();
    connect_a(0);
    CHECK(ap_leave(ap_a, other, IEEE80211_STYPE_DEAUTH, 7, 0) == 1 && n_seen == 0);
    CHECK(ap_leave(ap_a, sta, IEEE80211_STYPE_DISASSOC, 8, 0) == 1);
    CHECK(mlme.state == WLAN_STA_NONE && seen[0].ev.type == WLAN_EVENT_DISASSOCIATE);
    CHECK(seen[0].ev.reason == 8 && !seen[0].ev.locally_generated);
    connect_a(1);
    CHECK(wlan_mlme_deauthenticate(&mlme, ap_a, WLAN_REASON_DEAUTH_LEAVING, 0) == 0);
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_DEAUTH, ap_a));
    CHECK(sent[0].flags == WLAN_MLME_TX_PROTECT && get16(sent[0].f + HDR) == 3);
    CHECK(seen[0].ev.type == WLAN_EVENT_DEAUTHENTICATE && seen[0].ev.locally_generated);
    CHECK(mlme.state == WLAN_STA_NONE && bss_a.refs == 0);
    connect_a(0);
    CHECK(wlan_mlme_disassociate(&mlme, ap_a, WLAN_REASON_DISASSOC_STA_HAS_LEFT, 1) == 0);
    CHECK(n_sent == 0 && seen[0].ev.type == WLAN_EVENT_DISASSOCIATE);
    CHECK(seen[0].ev.locally_generated && mlme.state == WLAN_STA_NONE);
    hdr(f, IEEE80211_STYPE_BEACON, group, ap_a, ap_a);
    CHECK(deliver(f, HDR - 1, 0) == 1 && deliver(f, HDR, 0) == 0);
    put16(f, IEEE80211_FTYPE_DATA);
    CHECK(deliver(f, HDR, 0) == 0);
    hdr(f, IEEE80211_STYPE_PROBE_REQ, group, other, group);
    CHECK(deliver(f, HDR, 0) == 0);
}

/** Send an SAE frame of transaction and status with an n-byte body. */
static int sae_send(unsigned trans, unsigned status, unsigned n)
{
    struct wlan_mlme_auth req;
    uint8_t data[4 + 96];
    unsigned i;

    put16(data, trans);
    put16(data + 2, status);
    for (i = 0; i < n; ++i)
        data[4 + i] = (uint8_t)(trans * 16 + i);
    k_memset(&req, 0, sizeof(req));
    req.bss = &bss_a;
    req.algorithm = WLAN_AUTH_SAE;
    req.data = data;
    req.data_len = 4 + n;
    return wlan_mlme_authenticate(&mlme, &req);
}

/** Whether the last sent frame is SAE's of transaction and status. */
static int sent_sae(unsigned trans, unsigned status, unsigned n)
{
    const struct sent *s;

    s = &sent[n_sent - 1];
    return n_sent && sent_is(s, IEEE80211_STYPE_AUTH, ap_a) && s->len == HDR + 6 + n &&
           get16(s->f + HDR) == WLAN_AUTH_SAE && get16(s->f + HDR + 2) == trans &&
           get16(s->f + HDR + 4) == status && s->f[HDR + 6] == (uint8_t)(trans * 16);
}

/** Whether the last event carried the AP's SAE frame with status. */
static int carried(unsigned status)
{
    const struct seen *s;

    s = last_event();
    return s->ev.type == WLAN_EVENT_AUTHENTICATE && s->has_frame && s->ev.status == status &&
           get16(s->f + HDR) == WLAN_AUTH_SAE && get16(s->f + HDR + 4) == status;
}

/** SAE carried both ways: a hash-to-element commit, an anti-clogging
 * request, the AP's commit and confirm, then association. */
static void sae_exchange(void)
{
    reset();
    CHECK(sae_send(1, WLAN_STATUS_SAE_HASH_TO_ELEMENT, 96) == 0);
    CHECK(sent_sae(1, WLAN_STATUS_SAE_HASH_TO_ELEMENT, 96));
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 1, WLAN_STATUS_ANTI_CLOG_REQUIRED, 32) == 1);
    CHECK(carried(WLAN_STATUS_ANTI_CLOG_REQUIRED) && last_event()->ev.frame_len == HDR + 38);
    CHECK(mlme.state == WLAN_STA_AUTHENTICATING && mlme.waiting);
    wait_ms(WLAN_MLME_SAE_WAIT - 1);
    CHECK(n_sent == 1 && n_seen == 1);
    CHECK(sae_send(1, WLAN_STATUS_SAE_HASH_TO_ELEMENT, 64) == 0 && bss_a.refs == 1);
    CHECK(n_sent == 2 && sent_sae(1, WLAN_STATUS_SAE_HASH_TO_ELEMENT, 64));
    wait_ms(WLAN_MLME_SAE_TIMEOUT);
    CHECK(n_sent == 3 && sent_sae(1, WLAN_STATUS_SAE_HASH_TO_ELEMENT, 64));
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 1, WLAN_STATUS_SAE_HASH_TO_ELEMENT, 64) == 1);
    CHECK(carried(WLAN_STATUS_SAE_HASH_TO_ELEMENT) && mlme.state == WLAN_STA_AUTHENTICATING);
    CHECK(sae_send(2, WLAN_STATUS_SUCCESS, 34) == 0 && sent_sae(2, 0, 34));
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 1, WLAN_STATUS_SAE_HASH_TO_ELEMENT, 64) == 1);
    CHECK(n_seen == 2);
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 2, WLAN_STATUS_SUCCESS, 34) == 1);
    CHECK(carried(WLAN_STATUS_SUCCESS) && n_seen == 3 && mlme.state == WLAN_STA_AUTHENTICATED);
    CHECK(assoc(&bss_a, 0, 1) == 0);
    CHECK(ap_assoc(ap_a, IEEE80211_STYPE_ASSOC_RESP, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(mlme.state == WLAN_STA_ASSOCIATED && mlme.mfp);
}

/** SAE ends: the AP's confirm before ours, a status that ends the
 * attempt, and user space that does not answer. */
static void sae_endings(void)
{
    reset();
    CHECK(sae_send(1, WLAN_STATUS_SUCCESS, 8) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 1, WLAN_STATUS_SUCCESS, 8) == 1);
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 2, WLAN_STATUS_SUCCESS, 8) == 1);
    CHECK(carried(WLAN_STATUS_SUCCESS) && mlme.peer_confirmed);
    CHECK(mlme.state == WLAN_STA_AUTHENTICATING);
    CHECK(sae_send(2, WLAN_STATUS_SUCCESS, 8) == 0 && sent_sae(2, 0, 8));
    CHECK(mlme.state == WLAN_STA_AUTHENTICATED && n_seen == 2);
    reset();
    CHECK(sae_send(1, WLAN_STATUS_SUCCESS, 8) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 1, 77, 0) == 1);
    CHECK(carried(77) && mlme.state == WLAN_STA_NONE && bss_a.refs == 0);
    CHECK(queued() == 0);
    CHECK(sae_send(1, WLAN_STATUS_SUCCESS, 8) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1 && n_seen == 1);
    CHECK(ap_auth(ap_a, WLAN_AUTH_SAE, 1, WLAN_STATUS_SUCCESS, 8) == 1 && n_seen == 2);
    wait_ms(WLAN_MLME_SAE_WAIT - 1);
    CHECK(n_seen == 2 && n_sent == 2);
    wait_ms(1);
    CHECK(n_seen == 3 && !last_event()->has_frame && mlme.state == WLAN_STA_NONE);
    CHECK(last_event()->ev.type == WLAN_EVENT_AUTHENTICATE);
}

/** The station's state after a switch to frequency: still associated
 * with ap_a, nothing sent and nothing reported, the radio, the BSS, the
 * channel and the scan's home moved, and transmit no longer held. */
static int followed(unsigned frequency)
{
    return mlme.state == WLAN_STA_ASSOCIATED && mlme.bss == &bss_a && bss_a.refs == 1 &&
           n_sent == 0 && n_seen == 0 && configured == frequency && bss_a.frequency == frequency &&
           mlme.channel && mlme.channel->frequency == frequency && scan.home == mlme.channel &&
           moved_to == frequency && !held;
}

/**
 * The end of channel_switch, which calls it on channel 6 after three
 * moves: the public extended action sets a time that a beacon without the
 * element calls off, and a switch 20 beacons away outwaits a silent AP,
 * which is then probed and left on the new channel.
 */
static void channel_switch_timed(void)
{
    static const uint8_t ecsa1[4] = {0, 81, 1, 20}; /* back to 1 in 20 beacons */
    uint8_t f[HDR + 6];

    hdr(f, IEEE80211_STYPE_ACTION, sta, ap_a, ap_a);
    f[HDR] = WLAN_CATEGORY_PUBLIC;
    f[HDR + 1] = WLAN_PUB_ACTION_EXT_CHANSW_ANN;
    k_memcpy(f + HDR + 2, ecsa1, 4);
    f[HDR + 5] = 3;
    CHECK(deliver(f, HDR + 6, 0) == 1);
    CHECK(mlme.csa_channel && mlme.csa_channel->number == 1 && !held);
    wait_ms(100);
    CHECK(ap_beacon_with(ap_a, 0, 0, 0) == 0 && !mlme.csa_channel);
    wait_ms(300);
    CHECK(followed(2437) && moves == 3);

    /* A switch 20 beacons away outwaits a silent AP. */
    CHECK(ap_beacon_with(ap_a, WLAN_EID_EXT_CHANSWITCH, ecsa1, 4) == 0);
    wait_ms(2047);
    CHECK(n_sent == 0 && mlme.state == WLAN_STA_ASSOCIATED && moves == 3);
    wait_ms(1);
    CHECK(followed(2412) && moves == 4);
    wait_ms(2000);
    CHECK(n_sent == 3 && mlme.state == WLAN_STA_ASSOCIATED);
    wait_ms(500);
    CHECK(mlme.state == WLAN_STA_NONE && n_seen == 1);
    CHECK(seen[0].ev.type == WLAN_EVENT_DEAUTHENTICATE && seen[0].ev.frequency == 2412);
}

/**
 * The AP's channel switch: a Channel Switch Announcement of mode 1 in its
 * beacons holds transmit and moves the station at the count's end with no
 * leave; the AP then has the beacon-loss time on the new channel. An
 * extended announcement of count 0 and the spectrum-management action
 * frame of count 1 move it at once; the public extended action sets a time
 * that a beacon without the element calls off. A channel the band lacks is
 * ignored. While a switch waits, a silent AP is neither probed nor left.
 */
static void channel_switch(void)
{
    static const uint8_t csa6[3] = {1, 6, 5};        /* mode 1, channel 6, 5 beacons */
    static const uint8_t csa6_4[3] = {1, 6, 4};      /* the next beacon's count */
    static const uint8_t ecsa11[4] = {0, 81, 11, 0}; /* mode 0, class 81, channel 11, now */
    static const uint8_t csa14[3] = {0, 14, 2};      /* a channel the band lacks */
    uint8_t f[HDR + 2 + 5];
    unsigned pos;

    reset();
    connect_a(0);
    CHECK(configured == 2412);
    CHECK(ap_beacon_with(ap_a, WLAN_EID_CHANNEL_SWITCH, csa6, 3) == 0);
    CHECK(held && holds == 1 && mlme.csa_channel && mlme.csa_channel->number == 6);
    CHECK(configured == 2412 && moves == 0);
    wait_ms(102);
    CHECK(ap_beacon_with(ap_a, WLAN_EID_CHANNEL_SWITCH, csa6_4, 3) == 0);
    CHECK(holds == 1 && mlme.csa_at == now + 409);
    wait_ms(408);
    CHECK(moves == 0 && held && configured == 2412);
    wait_ms(1);
    CHECK(followed(2437) && moves == 1 && holds == 2 && !mlme.csa_channel);
    /* The AP heard on channel 6 keeps the association; its loss leaves. */
    wait_ms(700);
    CHECK(ap_beacon_with(ap_a, 0, 0, 0) == 0 && n_sent == 0);
    wait_ms(716);
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_PROBE_REQ, ap_a));
    CHECK(ap_beacon(ap_a, IEEE80211_STYPE_PROBE_RESP) == 0 && mlme.probes == 0);
    n_sent = 0;

    CHECK(ap_beacon_with(ap_a, WLAN_EID_EXT_CHANSWITCH, ecsa11, 4) == 0);
    CHECK(followed(2462) && moves == 2 && holds == 2);
    /* The AP's beacons on channel 11 may still name it: nothing changes. */
    CHECK(ap_beacon_with(ap_a, WLAN_EID_EXT_CHANSWITCH, ecsa11, 4) == 0 && moves == 2);

    hdr(f, IEEE80211_STYPE_ACTION, sta, ap_a, ap_a);
    f[HDR] = WLAN_CATEGORY_SPECTRUM_MGMT;
    f[HDR + 1] = WLAN_ACTION_SPCT_CHL_SWITCH;
    pos = HDR + 2;
    elem(f, &pos, WLAN_EID_CHANNEL_SWITCH, csa6, 3);
    f[pos - 1] = 1;
    CHECK(deliver(f, pos, 0) == 1);
    CHECK(followed(2437) && moves == 3 && holds == 4);

    CHECK(ap_beacon_with(ap_a, WLAN_EID_CHANNEL_SWITCH, csa14, 3) == 0);
    CHECK(!mlme.csa_channel && moves == 3 && configured == 2437);
    channel_switch_timed();
}

/** Stopping drops every state without frames or events. */
static void stopping(void)
{
    reset();
    connect_a(1);
    wlan_mlme_stop(&mlme);
    CHECK(mlme.state == WLAN_STA_NONE && n_sent == 0 && n_seen == 0);
    CHECK(bss_a.refs == 0 && queued() == 0);
}

/** Run the scripted AP's cases; no files or processes are made. */
int main(void)
{
    open_association();
    refusals();
    open_timeout();
    association_failures();
    reassociation();
    vanishing_ap();
    polled_ap();
    protected_frames();
    plain_frames();
    sae_exchange();
    sae_endings();
    channel_switch();
    stopping();
    if (failures)
        return 1;
    printf("wlan_mlme_hosttest: result: passed\n");
    return 0;
}
