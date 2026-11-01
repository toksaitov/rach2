/* Check nl80211 through the real generic-netlink dispatcher, wireless worker
 * and BSS table, against a stub station, with the requests wpa_supplicant
 * 2.12 and iw 6.17 send, and compare the answers' attributes.
 * host-deps: rach2/sys/network/socket/genetlink.c
 * host-deps: rach2/sys/network/wireless/work.c rach2/sys/network/wireless/bss.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 */
#include "../../rach2/sys/network/wireless/nl80211.c"
#include "../../rach2/sys/network/socket/netlink.h"
#include "../../rach2/sys/network/net.h"
#include "../../rach2/sys/lib/kirq.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            printf("nl80211_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

#define PORT               0x1234u /* the socket requests come from */
#define PORT2              0x2345u
#define DEAD_PORT          0x9999u /* a socket that has closed */
#define CTRL_CMD_GETFAMILY 3
#define NLMSG_DONE         3
#define F_REQUEST          0x001u
#define F_ACK              0x004u
#define F_DUMP             0x300u

/* ---- the machine around the code ---- */

static uint64_t now = 1000;
static uint32_t irq_flag = 1;
static int privileged = 1;
static int lock_depth;
static int detach_in_wait; /* the next wait detaches the second station */

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

/** Count nested network exclusion; nothing here may sleep inside it. */
uint32_t net_lock(void)
{
    return (uint32_t)lock_depth++;
}

/** Restore the preceding exclusion depth. */
void net_unlock(uint32_t flags)
{
    CHECK(lock_depth == (int)flags + 1);
    lock_depth = (int)flags;
}

/** The privilege the current row chose. */
int net_admin(void)
{
    return privileged;
}

/** Wakes are not needed: the wait below runs the worker itself. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* The station records nl80211 is given; it only compares their addresses. */
static struct wlan wlan_one, wlan_two, wlan_three;
#define WLAN1 (&wlan_one)
#define WLAN2 (&wlan_two)
#define WLAN3 (&wlan_three)

static wlan_event_fn handler;

/** The registry's: nl80211_attach installs its event consumer here. */
void wlan_set_event_handler(wlan_event_fn fn)
{
    handler = fn;
}

/** The registry's wlan_event, which passes a station's event to nl80211. */
void wlan_event(struct wlan *wlan, const struct wlan_event *event)
{
    CHECK(handler != 0);
    if (handler) {
        handler(wlan, event);
    }
}

/** Stand in for the sleep of a handler waiting on the worker: run the
 * worker until the condition holds. A row may detach the second station
 * first, as an unregistration would while the request waits. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(lock_depth == 0);
    if (detach_in_wait) {
        detach_in_wait = 0;
        nl80211_detach(WLAN2);
    }
    while (!cond(arg)) {
        if (!wlan_work_run_due(now)) {
            CHECK(!"a waited request never ran");
            return 0;
        }
    }
    return 0;
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

static unsigned slen(const char *s)
{
    unsigned n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16;
}

static void set16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void set32(uint8_t *p, uint32_t v)
{
    set16(p, v & 0xffff);
    set16(p + 2, v >> 16);
}

/* ---- the transport: what the dispatcher and nl80211 send ---- */

struct dgram {
    uint32_t to; /* port, or group for a multicast */
    unsigned len;
    uint8_t data[8192];
};

static struct dgram replies[16], events[16], unicasts[8];
static unsigned n_replies, n_events, n_unicasts, n_acks;
static int ack_error;

/** Keep a copy of a datagram in list. */
static int capture(struct dgram *list, unsigned *n, unsigned cap, uint32_t to, const uint8_t *data,
                   unsigned len)
{
    CHECK(len <= sizeof(list[0].data));
    if (*n < cap && len <= sizeof(list[0].data)) {
        list[*n].to = to;
        list[*n].len = len;
        k_memcpy(list[*n].data, data, len);
    }
    ++*n;
    return 0;
}

int netlink_genl_reply(struct socket *sock, uint32_t portid, const uint8_t *data, unsigned len)
{
    (void)sock;
    return capture(replies, &n_replies, 16, portid, data, len);
}

int netlink_genl_multicast(uint32_t group, const uint8_t *data, unsigned len)
{
    CHECK(lock_depth == 1);
    return capture(events, &n_events, 16, group, data, len);
}

int netlink_genl_unicast(uint32_t portid, const uint8_t *data, unsigned len)
{
    if (portid == DEAD_PORT) {
        return -ECONNREFUSED;
    }
    return capture(unicasts, &n_unicasts, 8, portid, data, len);
}

int netlink_ack(struct socket *sock, const uint8_t *req, int error, const char *detail,
                unsigned bad_offset)
{
    (void)sock;
    (void)req;
    (void)detail;
    (void)bad_offset;
    ++n_acks;
    ack_error = error;
    return 0;
}

static void clear_events(void)
{
    n_events = 0;
    n_unicasts = 0;
}

/* ---- requests, built as libnl builds them ---- */

static uint8_t req[4096];
static unsigned req_len;
static uint32_t seq;

static void rq_put(unsigned type, const void *data, unsigned len)
{
    unsigned size = (4 + len + 3) & ~3u;

    CHECK(req_len + size <= sizeof(req));
    k_memset(req + req_len, 0, size);
    set16(req + req_len, 4 + len);
    set16(req + req_len + 2, type);
    if (len) {
        k_memcpy(req + req_len + 4, data, len);
    }
    req_len += size;
}

static void rq_begin_family(unsigned type, unsigned cmd, unsigned flags)
{
    k_memset(req, 0, 20);
    req_len = 20;
    set16(req + 4, type);
    set16(req + 6, F_REQUEST | flags);
    set32(req + 8, ++seq);
    req[16] = (uint8_t)cmd;
    req[17] = 1;
}

/** Begin a request of nl80211's, a dump when dump is set. wpa_supplicant
 * asks for an acknowledgement of every one. */
static void rq_begin(unsigned cmd, int dump)
{
    rq_begin_family(family.id, cmd, dump ? F_DUMP : F_ACK);
}

static void rq_u8(unsigned type, uint8_t v)
{
    rq_put(type, &v, 1);
}

static void rq_u16(unsigned type, unsigned v)
{
    uint8_t b[2];

    set16(b, v);
    rq_put(type, b, 2);
}

static void rq_u32(unsigned type, uint32_t v)
{
    uint8_t b[4];

    set32(b, v);
    rq_put(type, b, 4);
}

static void rq_u64(unsigned type, uint64_t v)
{
    uint8_t b[8];

    set32(b, (uint32_t)v);
    set32(b + 4, (uint32_t)(v >> 32));
    rq_put(type, b, 8);
}

static void rq_flag(unsigned type)
{
    rq_put(type, 0, 0);
}

static unsigned rq_nest(unsigned type)
{
    unsigned at = req_len;

    rq_put(type | NLA_F_NESTED, 0, 0);
    return at;
}

static void rq_end(unsigned at)
{
    set16(req + at, req_len - at);
}

/** Send the request from port and return its outcome: the acknowledgement's
 * error, or a dump's, which its NLMSG_DONE carries. */
static int rq_send_from(uint32_t port)
{
    set32(req, req_len);
    n_replies = n_acks = 0;
    ack_error = 0;
    CHECK(genl_dispatch(0, port, req, req_len) == 0);
    CHECK(lock_depth == 0);
    if (n_acks) {
        return ack_error;
    }
    if (n_replies && n_replies <= 16 && get16(replies[n_replies - 1].data + 4) == NLMSG_DONE) {
        return (int)get32(replies[n_replies - 1].data + 16);
    }
    return 0;
}

static int rq_send(void)
{
    return rq_send_from(PORT);
}

/* ---- reading answers ---- */

#define TB_MAX 300
static const struct nlattr *tb[TB_MAX + 1];

/** Parse the one message of d into tb; returns its command, or -1. */
static int parse(const struct dgram *d)
{
    unsigned len = get32(d->data);

    if (len < 20 || len > d->len || get16(d->data + 4) != family.id) {
        k_memset(tb, 0, sizeof(tb));
        return -1;
    }
    CHECK(nla_parse(d->data + 20, len - 20, 0, TB_MAX, tb, 0) == 0);
    return d->data[16];
}

/** Parse a nest into t, of max+1 entries. */
static void parse_nest(const struct nlattr *a, const struct nlattr **t, unsigned max)
{
    CHECK(a != 0);
    if (a) {
        CHECK(nla_parse_nested(a, 0, max, t, 0) == 0);
    } else {
        k_memset(t, 0, (max + 1) * sizeof(*t));
    }
}

static uint32_t u32_of(const struct nlattr *a)
{
    CHECK(a && nla_len(a) == 4);
    return a && nla_len(a) == 4 ? nla_get_u32(a) : 0xdeadbeefu;
}

static unsigned u16_of(const struct nlattr *a)
{
    CHECK(a && nla_len(a) == 2);
    return a && nla_len(a) == 2 ? nla_get_u16(a) : 0xdeadu;
}

static int is_flag(const struct nlattr *a)
{
    return a && nla_len(a) == 0;
}

static int has_bytes(const struct nlattr *a, const void *data, unsigned len)
{
    return a && nla_len(a) == len && same(nla_data(a), data, len);
}

/* ---- the station below nl80211 ---- */

enum call_kind {
    CALL_NONE,
    CALL_SCAN,
    CALL_ABORT,
    CALL_AUTH,
    CALL_ASSOC,
    CALL_DEAUTH,
    CALL_DISASSOC,
    CALL_AUTHORIZE,
    CALL_ADD_KEY,
    CALL_DEL_KEY,
    CALL_DEFAULT_KEY,
    CALL_TX
};

static struct {
    enum call_kind kind;
    struct wlan *wlan;
    struct wlan_scan_request scan;
    struct nl80211_auth auth;
    struct nl80211_assoc assoc;
    uint8_t ies[NL80211_MAX_IE];
    uint8_t data[256];
    struct wlan_key key;
    uint8_t addr[6];
    unsigned reason, index, frequency;
    int local, flag;
    uint64_t cookie;
} last;
static unsigned n_calls;
static int next_rc;
static struct nl80211_status status_now;

/** Note a call and return the result the row scripted, once. */
static int called(struct wlan *wlan, enum call_kind kind)
{
    int rc = next_rc;

    next_rc = 0;
    last.kind = kind;
    last.wlan = wlan;
    ++n_calls;
    return rc;
}

static int st_scan(struct wlan *wlan, const struct wlan_scan_request *r)
{
    last.scan = *r;
    return called(wlan, CALL_SCAN);
}

static int st_abort(struct wlan *wlan)
{
    return called(wlan, CALL_ABORT);
}

static int st_auth(struct wlan *wlan, const struct nl80211_auth *a)
{
    last.auth = *a;
    CHECK(a->data_len <= sizeof(last.data) && a->ies_len <= sizeof(last.ies));
    k_memcpy(last.data, a->data, a->data_len);
    k_memcpy(last.ies, a->ies, a->ies_len);
    return called(wlan, CALL_AUTH);
}

static int st_assoc(struct wlan *wlan, const struct nl80211_assoc *a)
{
    last.assoc = *a;
    CHECK(a->ies_len <= sizeof(last.ies));
    k_memcpy(last.ies, a->ies, a->ies_len);
    return called(wlan, CALL_ASSOC);
}

static int st_deauth(struct wlan *wlan, const uint8_t bssid[6], unsigned reason, int local)
{
    k_memcpy(last.addr, bssid, 6);
    last.reason = reason;
    last.local = local;
    return called(wlan, CALL_DEAUTH);
}

static int st_disassoc(struct wlan *wlan, const uint8_t bssid[6], unsigned reason, int local)
{
    k_memcpy(last.addr, bssid, 6);
    last.reason = reason;
    last.local = local;
    return called(wlan, CALL_DISASSOC);
}

static int st_authorize(struct wlan *wlan, const uint8_t peer[6], int authorized)
{
    k_memcpy(last.addr, peer, 6);
    last.flag = authorized;
    return called(wlan, CALL_AUTHORIZE);
}

static int st_add_key(struct wlan *wlan, const struct wlan_key *key)
{
    last.key = *key;
    return called(wlan, CALL_ADD_KEY);
}

static int st_del_key(struct wlan *wlan, const uint8_t peer[6], unsigned index)
{
    k_memcpy(last.addr, peer, 6);
    last.index = index;
    return called(wlan, CALL_DEL_KEY);
}

static int st_default_key(struct wlan *wlan, unsigned index, int management)
{
    last.index = index;
    last.flag = management;
    return called(wlan, CALL_DEFAULT_KEY);
}

static int st_tx(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned frequency,
                 uint64_t cookie)
{
    CHECK(len <= sizeof(last.data));
    k_memcpy(last.data, frame, len);
    last.index = len;
    last.frequency = frequency;
    last.cookie = cookie;
    return called(wlan, CALL_TX);
}

static void st_status(struct wlan *wlan, struct nl80211_status *out)
{
    (void)wlan;
    *out = status_now;
}

static const struct nl80211_ops station_ops = {
    st_scan,    st_abort,   st_auth,        st_assoc, st_deauth, st_disassoc, st_authorize,
    st_add_key, st_del_key, st_default_key, st_tx,    st_status, 0,           0};

/* ---- the radio, its interface and the BSS table ---- */

static struct wlan_radio radio, radio2;
static struct wlan_band band;
static struct netdev nd, nd2;
static struct wlan_bss_table table, table2;
static const uint8_t ap1[6] = {0x0a, 0, 0, 0, 0, 1};
static const uint8_t ap2[6] = {0x0a, 0, 0, 0, 0, 2};
static const uint8_t ap3[6] = {0x0a, 0, 0, 0, 0, 3};
static const uint8_t mine[6] = {0x02, 0, 0, 0, 0, 1};
static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/** The world regulatory domain as the registry narrows a radio's band:
 * channels 1 to 11 active, 12 and 13 passive, 14 off. */
static void make_band(void)
{
    static const uint16_t rates[12] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};
    unsigned i;

    k_memset(&band, 0, sizeof(band));
    for (i = 0; i < 14; ++i) {
        band.channels[i].frequency = (uint16_t)(2412 + 5 * i);
        band.channels[i].number = (uint8_t)(i + 1);
        band.channels[i].max_power = 20;
    }
    band.channels[11].flags = WLAN_CHANNEL_PASSIVE;
    band.channels[12].flags = WLAN_CHANNEL_PASSIVE;
    band.channels[13].frequency = 2484;
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

/** Build a beacon of bssid with ssid on channel number into frame. */
static unsigned beacon(uint8_t *frame, const uint8_t *bssid, const char *ssid, unsigned number)
{
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96};
    uint8_t ds = (uint8_t)number;
    unsigned used = 36;

    k_memset(frame, 0, 36);
    frame[0] = 0x80;
    k_memset(frame + 4, 0xff, 6);
    k_memcpy(frame + 10, bssid, 6);
    k_memcpy(frame + 16, bssid, 6);
    frame[24] = 0x42; /* TSF 0x42 */
    frame[32] = 100;  /* beacon interval */
    frame[34] = 0x11; /* ESS, privacy */
    frame[35] = 0x04; /* short slot time */
    CHECK(ieee80211_put_elem(frame, 512, &used, WLAN_EID_SSID, (const uint8_t *)ssid, slen(ssid)) ==
          0);
    CHECK(ieee80211_put_elem(frame, 512, &used, WLAN_EID_SUPP_RATES, rates, 4) == 0);
    CHECK(ieee80211_put_elem(frame, 512, &used, WLAN_EID_DS_PARAMS, &ds, 1) == 0);
    return used;
}

/** Put a beacon heard at signal dBm into the table. */
static void hear(const uint8_t *bssid, const char *ssid, unsigned number, int signal)
{
    struct wlan_rx_status rx;
    uint8_t frame[512];
    unsigned len = beacon(frame, bssid, ssid, number);

    k_memset(&rx, 0, sizeof(rx));
    rx.frequency = (uint16_t)(2407 + 5 * number);
    rx.signal = (int16_t)signal;
    rx.hw_key = -1;
    CHECK(wlan_bss_rx(&table, &band, frame, len, &rx, now) == 0);
}

/** A management frame of subtype stype from the AP to the station, with
 * body; returns its length. */
static unsigned mgmt(uint8_t *frame, unsigned stype, const uint8_t *from, const uint8_t *body,
                     unsigned body_len)
{
    k_memset(frame, 0, 24);
    frame[0] = (uint8_t)stype;
    k_memcpy(frame + 4, mine, 6);
    k_memcpy(frame + 10, from, 6);
    k_memcpy(frame + 16, from, 6);
    k_memcpy(frame + 24, body, body_len);
    return 24 + body_len;
}

/** Deliver an event of type about peer, as the station layer would. */
static void event(enum wlan_event_type type, const uint8_t *peer, const uint8_t *frame,
                  unsigned len, unsigned status, unsigned reason, int local)
{
    struct wlan_event ev;

    k_memset(&ev, 0, sizeof(ev));
    ev.type = type;
    if (peer) {
        k_memcpy(ev.peer, peer, 6);
    }
    ev.frame = frame;
    ev.frame_len = len;
    ev.status = (uint16_t)status;
    ev.reason = (uint16_t)reason;
    ev.locally_generated = local;
    ev.frequency = 2437;
    wlan_event(WLAN1, &ev);
}

static void scan_ev(enum wlan_event_type type, uint64_t cookie)
{
    struct wlan_event ev;

    k_memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.cookie = cookie;
    wlan_event(WLAN1, &ev);
}

/** The first event of group and command since the last clear, or NULL;
 * tb then holds its attributes. */
static const struct dgram *find_event(unsigned group, unsigned cmd)
{
    unsigned i;

    for (i = 0; i < n_events && i < 16; ++i) {
        if (events[i].to == groups[group].id && parse(&events[i]) == (int)cmd) {
            return &events[i];
        }
    }
    k_memset(tb, 0, sizeof(tb));
    return 0;
}

static void set_associated(const uint8_t *bssid, enum wlan_sta_state state)
{
    k_memset(&status_now, 0, sizeof(status_now));
    status_now.state = state;
    k_memcpy(status_now.bssid, bssid, 6);
    status_now.frequency = 2437;
    k_memcpy(status_now.ssid.data, "lab", 3);
    status_now.ssid.len = 3;
    status_now.ht = 1;
    status_now.qos = 1;
    status_now.assoc_at = now;
    status_now.last_rx = now;
    status_now.counters.signal = -4000;
    status_now.counters.noise = -9500;
    status_now.counters.rx_bytes = 1000;
    status_now.counters.tx_bytes = 2000;
    status_now.counters.missed_beacons = 3;
}

static void setup(void)
{
    make_band();
    k_memset(&radio, 0, sizeof(radio));
    k_memcpy(radio.mac, mine, 6);
    radio.band = band;
    radio.country[0] = radio.country[1] = '0';
    radio.index = 0;
    k_memset(&nd, 0, sizeof(nd));
    k_memcpy(nd.name, "wlan0", 6);
    nd.index = 2;
    k_memcpy(nd.mac, mine, 6);
    wlan_bss_init(&table);
    CHECK(nl80211_attach(WLAN1, &radio, &band, &nd, &table, &station_ops) == 0);
    CHECK(family.id != 0);
}

/* ---- the rows ---- */

/** libnl's genl_ctrl_resolve and its group lookups, as wpa_supplicant
 * starts. */
static void resolve(void)
{
    static const char *names[5] = {"config", "scan", "regulatory", "mlme", "vendor"};
    const struct nlattr *t[8], *g[8], *one[3];
    unsigned i;

    rq_begin_family(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, F_ACK);
    rq_put(2, "nl80211", 8);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 1);
    CHECK(get16(replies[0].data + 4) == GENL_ID_CTRL);
    CHECK(nla_parse(replies[0].data + 20, get32(replies[0].data) - 20, 0, 7, t, 0) == 0);
    CHECK(u16_of(t[1]) == family.id);
    CHECK(has_bytes(t[2], "nl80211", 8));
    CHECK(u32_of(t[3]) == 1);
    parse_nest(t[7], g, 7);
    for (i = 0; i < 5; ++i) {
        parse_nest(g[i + 1], one, 2);
        CHECK(has_bytes(one[1], names[i], slen(names[i]) + 1));
        CHECK(u32_of(one[2]) == groups[i].id && groups[i].id != 0);
    }
}

static void protocol_features(void)
{
    rq_begin(NL80211_CMD_GET_PROTOCOL_FEATURES, 0);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 1 && parse(&replies[0]) == NL80211_CMD_GET_PROTOCOL_FEATURES);
    CHECK(u32_of(tb[NL80211_ATTR_PROTOCOL_FEATURES]) == NL80211_PROTOCOL_FEATURE_SPLIT_WIPHY_DUMP);
}

/** Check a band as wpa_supplicant's phy_info_band reads it. */
static void check_band(const struct nlattr *bands)
{
    const struct nlattr *b[2], *t[7], *list[16], *item[11];
    unsigned i;

    parse_nest(bands, b, 1);
    parse_nest(b[NL80211_BAND_2GHZ], t, 6);
    CHECK(u16_of(t[NL80211_BAND_ATTR_HT_CAPA]) == 0x012c);
    CHECK(t[NL80211_BAND_ATTR_HT_MCS_SET] && nla_len(t[NL80211_BAND_ATTR_HT_MCS_SET]) == 16 &&
          nla_data(t[NL80211_BAND_ATTR_HT_MCS_SET])[0] == 0xff);
    CHECK(t[NL80211_BAND_ATTR_HT_AMPDU_FACTOR] &&
          nla_get_u8(t[NL80211_BAND_ATTR_HT_AMPDU_FACTOR]) == 3);
    parse_nest(t[NL80211_BAND_ATTR_FREQS], list, 15);
    for (i = 0; i < 14; ++i) {
        parse_nest(list[i], item, 10);
        CHECK(u32_of(item[NL80211_FREQUENCY_ATTR_FREQ]) == (i < 13 ? 2412 + 5 * i : 2484));
        CHECK(is_flag(item[NL80211_FREQUENCY_ATTR_DISABLED]) == (i == 13));
        CHECK(is_flag(item[NL80211_FREQUENCY_ATTR_NO_IR]) == (i == 11 || i == 12));
        if (i < 13) {
            CHECK(u32_of(item[NL80211_FREQUENCY_ATTR_MAX_TX_POWER]) == 2000);
        }
    }
    CHECK(list[14] == 0);
    parse_nest(t[NL80211_BAND_ATTR_RATES], list, 15);
    for (i = 0; i < 12; ++i) {
        parse_nest(list[i], item, 2);
        CHECK(u32_of(item[NL80211_BITRATE_ATTR_RATE]) == band.rates[i]);
        CHECK(is_flag(item[NL80211_BITRATE_ATTR_2GHZ_SHORTPREAMBLE]) == (i >= 1 && i <= 3));
    }
}

/** Whether the SUPPORTED_COMMANDS nest lists cmd. */
static int lists_command(const struct nlattr *nest, unsigned cmd)
{
    const struct nlattr *t[16];
    unsigned i;

    parse_nest(nest, t, 15);
    for (i = 1; i < 16; ++i) {
        if (t[i] && nla_get_u32(t[i]) == cmd) {
            return 1;
        }
    }
    return 0;
}

/** Check the attributes of the first part, as wiphy_info_handler reads. */
static void check_basics(void)
{
    const struct nlattr *t[NL80211_IFTYPE_STATION + 1];

    CHECK(tb[NL80211_ATTR_MAX_NUM_SCAN_SSIDS] &&
          nla_get_u8(tb[NL80211_ATTR_MAX_NUM_SCAN_SSIDS]) == WLAN_MAX_SCAN_SSIDS);
    CHECK(u16_of(tb[NL80211_ATTR_MAX_SCAN_IE_LEN]) == WLAN_MAX_SCAN_IE);
    parse_nest(tb[NL80211_ATTR_SUPPORTED_IFTYPES], t, NL80211_IFTYPE_STATION);
    CHECK(is_flag(t[NL80211_IFTYPE_STATION]));
    CHECK(has_bytes(tb[NL80211_ATTR_CIPHER_SUITES], ciphers, 12));
    CHECK(u32_of(tb[NL80211_ATTR_FEATURE_FLAGS]) & NL80211_FEATURE_SAE);
}

/** GET_WIPHY as wpa_supplicant asks for it: a split dump of the wiphy of
 * its interface. */
static void wiphy_split(void)
{
    const struct nlattr *t[4], *frames[1];
    unsigned i;

    rq_begin(NL80211_CMD_GET_WIPHY, 1);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_flag(NL80211_ATTR_SPLIT_WIPHY_DUMP);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 5);
    for (i = 0; i < 4; ++i) {
        CHECK(parse(&replies[i]) == NL80211_CMD_NEW_WIPHY);
        CHECK(get16(replies[i].data + 6) & 2); /* NLM_F_MULTI */
        CHECK(u32_of(tb[NL80211_ATTR_WIPHY]) == 0);
        CHECK(has_bytes(tb[NL80211_ATTR_WIPHY_NAME], "phy0", 5));
        CHECK(u32_of(tb[NL80211_ATTR_GENERATION]) == generation);
        if (i == 0) {
            check_basics();
        } else if (i == 1) {
            check_band(tb[NL80211_ATTR_WIPHY_BANDS]);
        } else if (i == 2) {
            CHECK(lists_command(tb[NL80211_ATTR_SUPPORTED_COMMANDS], NL80211_CMD_AUTHENTICATE));
            CHECK(lists_command(tb[NL80211_ATTR_SUPPORTED_COMMANDS], NL80211_CMD_ASSOCIATE));
            CHECK(lists_command(tb[NL80211_ATTR_SUPPORTED_COMMANDS], NL80211_CMD_CONNECT));
            CHECK(!lists_command(tb[NL80211_ATTR_SUPPORTED_COMMANDS], NL80211_CMD_NEW_INTERFACE));
        } else {
            parse_nest(tb[NL80211_ATTR_TX_FRAME_TYPES], t, 3);
            parse_nest(t[NL80211_IFTYPE_STATION], frames, 0);
            CHECK(tb[NL80211_ATTR_RX_FRAME_TYPES] != 0);
            CHECK(has_bytes(
                nla_data(t[NL80211_IFTYPE_STATION])
                    ? (const struct nlattr *)(const void *)nla_data(t[NL80211_IFTYPE_STATION])
                    : 0,
                "\xd0\x00", 2));
        }
    }
    CHECK(get16(replies[4].data + 4) == NLMSG_DONE);
}

/** GET_WIPHY whole: by WIPHY as iw phy0 info asks, as an unsplit dump, and
 * the refusals. */
static void wiphy_whole(void)
{
    rq_begin(NL80211_CMD_GET_WIPHY, 0);
    rq_u32(NL80211_ATTR_WIPHY, 0);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 1 && parse(&replies[0]) == NL80211_CMD_NEW_WIPHY);
    check_basics();
    check_band(tb[NL80211_ATTR_WIPHY_BANDS]);
    CHECK(lists_command(tb[NL80211_ATTR_SUPPORTED_COMMANDS], NL80211_CMD_FRAME));
    CHECK(tb[NL80211_ATTR_TX_FRAME_TYPES] != 0);

    rq_begin(NL80211_CMD_GET_WIPHY, 1);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 2 && parse(&replies[0]) == NL80211_CMD_NEW_WIPHY);
    check_basics();
    CHECK(tb[NL80211_ATTR_WIPHY_BANDS] != 0 && tb[NL80211_ATTR_SUPPORTED_COMMANDS] != 0);

    rq_begin(NL80211_CMD_GET_WIPHY, 1);
    rq_u32(NL80211_ATTR_IFINDEX, 9);
    CHECK(rq_send() == -ENODEV);
    rq_begin(NL80211_CMD_GET_WIPHY, 0);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_GET_WIPHY, 0);
    rq_u32(NL80211_ATTR_WIPHY, 7);
    CHECK(rq_send() == -ENODEV);
}

/** GET_INTERFACE as wpa_supplicant reads its wiphy and mode, and iw dev;
 * SET_INTERFACE as wpa_supplicant sets station mode; NEW_INTERFACE as iw
 * interface add asks. */
static void interfaces(void)
{
    rq_begin(NL80211_CMD_GET_INTERFACE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 1 && parse(&replies[0]) == NL80211_CMD_NEW_INTERFACE);
    CHECK(has_bytes(tb[NL80211_ATTR_IFNAME], "wlan0", 6));
    CHECK(u32_of(tb[NL80211_ATTR_IFTYPE]) == NL80211_IFTYPE_STATION);
    CHECK(u32_of(tb[NL80211_ATTR_WIPHY]) == 0);
    CHECK(tb[NL80211_ATTR_WDEV] && nla_get_u64(tb[NL80211_ATTR_WDEV]) == 1);
    CHECK(has_bytes(tb[NL80211_ATTR_MAC], mine, 6));
    CHECK(tb[NL80211_ATTR_WIPHY_FREQ] == 0 && tb[NL80211_ATTR_SSID] == 0);

    set_associated(ap1, WLAN_STA_AUTHORIZED);
    rq_begin(NL80211_CMD_GET_INTERFACE, 0);
    rq_u64(NL80211_ATTR_WDEV, 1);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 1 && parse(&replies[0]) == NL80211_CMD_NEW_INTERFACE);
    CHECK(u32_of(tb[NL80211_ATTR_WIPHY_FREQ]) == 2437);
    CHECK(u32_of(tb[NL80211_ATTR_CHANNEL_WIDTH]) == NL80211_CHAN_WIDTH_20);
    CHECK(u32_of(tb[NL80211_ATTR_WIPHY_CHANNEL_TYPE]) == NL80211_CHAN_HT20);
    CHECK(has_bytes(tb[NL80211_ATTR_SSID], "lab", 3));
    k_memset(&status_now, 0, sizeof(status_now));

    rq_begin(NL80211_CMD_GET_INTERFACE, 1);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 2 && parse(&replies[0]) == NL80211_CMD_NEW_INTERFACE);
    CHECK(get16(replies[1].data + 4) == NLMSG_DONE);

    rq_begin(NL80211_CMD_GET_INTERFACE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 1);
    CHECK(rq_send() == -ENODEV);

    rq_begin(NL80211_CMD_SET_INTERFACE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u32(NL80211_ATTR_IFTYPE, NL80211_IFTYPE_STATION);
    CHECK(rq_send() == 0);
    rq_begin(NL80211_CMD_SET_INTERFACE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u32(NL80211_ATTR_IFTYPE, 3); /* AP */
    CHECK(rq_send() == -EOPNOTSUPP);
    rq_begin(NL80211_CMD_SET_INTERFACE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u8(NL80211_ATTR_4ADDR, 1);
    CHECK(rq_send() == -EOPNOTSUPP);
    privileged = 0;
    rq_begin(NL80211_CMD_SET_INTERFACE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == -EPERM);
    privileged = 1;

    rq_begin(NL80211_CMD_NEW_INTERFACE, 0);
    rq_u32(NL80211_ATTR_WIPHY, 0);
    rq_u32(NL80211_ATTR_IFTYPE, NL80211_IFTYPE_STATION);
    CHECK(rq_send() == -EOPNOTSUPP);
    rq_begin(8, 0); /* DEL_INTERFACE */
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == -EOPNOTSUPP);
    rq_begin(63, 0); /* SET_CQM, which bgscan tries first */
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == -EOPNOTSUPP);
}

/** Check that the nests of a scan event say SSID "" or "lab" and the
 * frequencies 2412 and 2437. */
static void check_scan_request(const char *ssid)
{
    const struct nlattr *t[4];

    parse_nest(tb[NL80211_ATTR_SCAN_SSIDS], t, 3);
    CHECK(has_bytes(t[0], ssid, slen(ssid)) && t[1] == 0);
    parse_nest(tb[NL80211_ATTR_SCAN_FREQUENCIES], t, 3);
    CHECK(u32_of(t[0]) == 2412 && u32_of(t[1]) == 2437 && t[2] == 0);
}

/** TRIGGER_SCAN as wpa_supplicant sends it, its events, GET_SCAN as it
 * and iw read the table, and the refusals. */
static void scan(void)
{
    static const uint8_t ie[4] = {0x7f, 2, 0, 0};
    const struct nlattr *b[NL80211_BSS_LAST_SEEN_BOOTTIME + 1];
    unsigned nest, i, found = 0;
    uint64_t cookie;

    now = 500;
    hear(ap3, "old", 1, -70); /* heard before the scan; the flush removes it */
    now = 1000;
    n_calls = 0;
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_SCAN_SSIDS);
    rq_put(1, 0, 0);
    rq_end(nest);
    nest = rq_nest(NL80211_ATTR_SCAN_FREQUENCIES);
    rq_u32(1, 2412);
    rq_u32(2, 2437);
    rq_u32(3, 2484); /* off: left out */
    rq_u32(4, 2412); /* again: left out */
    rq_end(nest);
    rq_put(NL80211_ATTR_IE, ie, 4);
    rq_u32(NL80211_ATTR_SCAN_FLAGS, NL80211_SCAN_FLAG_FLUSH);
    CHECK(rq_send() == 0);
    CHECK(n_calls == 1 && last.kind == CALL_SCAN && last.wlan == WLAN1);
    CHECK(last.scan.n_ssids == 1 && last.scan.ssids[0].len == 0);
    CHECK(last.scan.n_frequencies == 2 && last.scan.frequencies[0] == 2412 &&
          last.scan.frequencies[1] == 2437);
    CHECK(last.scan.ies_len == 4 && same(last.scan.ies, ie, 4));
    CHECK(last.scan.cookie != 0);
    cookie = last.scan.cookie;

    clear_events();
    scan_ev(WLAN_EVENT_SCAN_STARTED, cookie);
    CHECK(find_event(GROUP_SCAN, NL80211_CMD_TRIGGER_SCAN) != 0);
    CHECK(u32_of(tb[NL80211_ATTR_IFINDEX]) == 2 && u32_of(tb[NL80211_ATTR_WIPHY]) == 0);
    check_scan_request("");
    CHECK(has_bytes(tb[NL80211_ATTR_IE], ie, 4));
    CHECK(u32_of(tb[NL80211_ATTR_SCAN_FLAGS]) == NL80211_SCAN_FLAG_FLUSH);

    now = 1100;
    hear(ap1, "lab", 6, -40);
    hear(ap2, "lab", 1, -60);
    CHECK(table.count == 3);
    clear_events();
    scan_ev(WLAN_EVENT_SCAN_DONE, cookie);
    CHECK(table.count == 2);
    CHECK(find_event(GROUP_SCAN, NL80211_CMD_NEW_SCAN_RESULTS) != 0);
    CHECK(u32_of(tb[NL80211_ATTR_IFINDEX]) == 2);

    now = 1150;
    set_associated(ap1, WLAN_STA_ASSOCIATED);
    rq_begin(NL80211_CMD_GET_SCAN, 1);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 3);
    for (i = 0; i < 2; ++i) {
        CHECK(parse(&replies[i]) == NL80211_CMD_NEW_SCAN_RESULTS);
        CHECK(u32_of(tb[NL80211_ATTR_GENERATION]) == table.generation);
        parse_nest(tb[NL80211_ATTR_BSS], b, NL80211_BSS_LAST_SEEN_BOOTTIME);
        CHECK(u32_of(b[NL80211_BSS_SEEN_MS_AGO]) == 50);
        CHECK(u16_of(b[NL80211_BSS_BEACON_INTERVAL]) == 100);
        CHECK(u16_of(b[NL80211_BSS_CAPABILITY]) == 0x0411);
        CHECK(b[NL80211_BSS_TSF] && nla_get_u64(b[NL80211_BSS_TSF]) == 0x42);
        CHECK(b[NL80211_BSS_LAST_SEEN_BOOTTIME] &&
              nla_get_u64(b[NL80211_BSS_LAST_SEEN_BOOTTIME]) == (uint64_t)1100 * 1000000);
        CHECK(b[NL80211_BSS_INFORMATION_ELEMENTS] &&
              nla_data(b[NL80211_BSS_INFORMATION_ELEMENTS])[0] == WLAN_EID_SSID);
        if (has_bytes(b[NL80211_BSS_BSSID], ap1, 6)) {
            ++found;
            CHECK(u32_of(b[NL80211_BSS_FREQUENCY]) == 2437);
            CHECK((int32_t)u32_of(b[NL80211_BSS_SIGNAL_MBM]) == -4000);
            CHECK(u32_of(b[NL80211_BSS_STATUS]) == NL80211_BSS_STATUS_ASSOCIATED);
        } else {
            CHECK(has_bytes(b[NL80211_BSS_BSSID], ap2, 6));
            CHECK(u32_of(b[NL80211_BSS_FREQUENCY]) == 2412);
            CHECK(b[NL80211_BSS_STATUS] == 0);
        }
    }
    CHECK(found == 1);
    CHECK(get16(replies[2].data + 4) == NLMSG_DONE);
    k_memset(&status_now, 0, sizeof(status_now));

    rq_begin(NL80211_CMD_GET_SCAN, 1);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_GET_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == -EOPNOTSUPP);

    /* A scan nl80211 did not start is described by its interface alone. */
    clear_events();
    scan_ev(WLAN_EVENT_SCAN_STARTED, cookie + 100);
    CHECK(find_event(GROUP_SCAN, NL80211_CMD_TRIGGER_SCAN) != 0);
    CHECK(tb[NL80211_ATTR_SCAN_SSIDS] == 0 && u32_of(tb[NL80211_ATTR_IFINDEX]) == 2);

    rq_begin(NL80211_CMD_ABORT_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0 && last.kind == CALL_ABORT);
    clear_events();
    scan_ev(WLAN_EVENT_SCAN_ABORTED, cookie);
    CHECK(find_event(GROUP_SCAN, NL80211_CMD_SCAN_ABORTED) != 0);
    check_scan_request("");

    /* Refusals: five SSIDs, a long SSID, no channel left, long elements, a
     * flag this station lacks, and the station's own -EBUSY. */
    n_calls = 0;
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_SCAN_SSIDS);
    for (i = 1; i <= 5; ++i) {
        rq_put(i, "x", 1);
    }
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_SCAN_SSIDS);
    rq_put(1, "0123456789abcdef0123456789abcdefX", 33);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_SCAN_FREQUENCIES);
    rq_u32(1, 2484);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_IE, req + 2000, WLAN_MAX_SCAN_IE + 1);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u32(NL80211_ATTR_SCAN_FLAGS, 8); /* RANDOM_ADDR */
    CHECK(rq_send() == -EOPNOTSUPP);
    CHECK(n_calls == 0);
    next_rc = -EBUSY;
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == -EBUSY && n_calls == 1);
    CHECK(last.scan.n_ssids == 0 && last.scan.n_frequencies == 0);
}

static const uint8_t rsn[22] = {0x30, 20,   1, 0, 0x00, 0x0f, 0xac, 4,    1, 0,    0x00,
                                0x0f, 0xac, 4, 1, 0,    0x00, 0x0f, 0xac, 2, 0x00, 0x00};

/** An association response of status with an SSID-free element list. */
static unsigned assoc_resp(uint8_t *frame, unsigned status)
{
    uint8_t body[12] = {0x11, 0x04, 0, 0, 1, 0xc0, 0x01, 4, 0x82, 0x84, 0x8b, 0x96};

    body[2] = (uint8_t)status;
    return mgmt(frame, 0x10, ap1, body, sizeof(body));
}

/** Put the attributes wpa_supplicant's ASSOCIATE carries for WPA2-PSK. */
static void assoc_request(void)
{
    uint32_t suite = WLAN_CIPHER_SUITE_CCMP, akm = WLAN_AKM_SUITE_PSK;

    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_put(NL80211_ATTR_IE, rsn, sizeof(rsn));
    rq_u32(NL80211_ATTR_WPA_VERSIONS, 2);
    rq_put(NL80211_ATTR_CIPHER_SUITES_PAIRWISE, &suite, 4);
    rq_u32(NL80211_ATTR_CIPHER_SUITE_GROUP, WLAN_CIPHER_SUITE_CCMP);
    rq_put(NL80211_ATTR_AKM_SUITES, &akm, 4);
    rq_flag(NL80211_ATTR_CONTROL_PORT);
    rq_u16(NL80211_ATTR_CONTROL_PORT_ETHERTYPE, 0x888e);
    rq_flag(NL80211_ATTR_CONTROL_PORT_NO_ENCRYPT);
    rq_u32(NL80211_ATTR_USE_MFP, NL80211_MFP_NO);
}

/** Put AUTHENTICATE's attributes for ap1 on channel 6. */
static void auth_request(unsigned type)
{
    rq_begin(NL80211_CMD_AUTHENTICATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_u32(NL80211_ATTR_AUTH_TYPE, type);
}

/** wpa_supplicant's SME joining WPA2-PSK: authentication, association,
 * the keys and the port, then the AP's deauthentication. */
static void join_wpa2(void)
{
    static const uint8_t auth_ok[6] = {0, 0, 2, 0, 0, 0};
    static const uint8_t deauth_7[2] = {7, 0};
    static const uint8_t tk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static const uint8_t rsc[6] = {1, 2, 3, 4, 5, 6};
    const struct nlattr *t[NL80211_STA_INFO_RX_DROP_MISC + 1], *s[NL80211_SURVEY_INFO_IN_USE + 1];
    uint8_t frame[64];
    unsigned len, nest, types;
    uint32_t flags[2];

    auth_request(NL80211_AUTHTYPE_OPEN_SYSTEM);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_AUTH && same(last.auth.bssid, ap1, 6));
    CHECK(last.auth.frequency == 2437 && last.auth.ssid.len == 3);
    CHECK(last.auth.algorithm == WLAN_AUTH_OPEN && last.auth.data_len == 0);

    clear_events();
    len = mgmt(frame, 0xb0, ap1, auth_ok, 6);
    event(WLAN_EVENT_AUTHENTICATE, ap1, frame, len, 0, 0, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_AUTHENTICATE) != 0);
    CHECK(has_bytes(tb[NL80211_ATTR_FRAME], frame, len));
    CHECK(u32_of(tb[NL80211_ATTR_IFINDEX]) == 2);
    CHECK(n_events == 1);

    assoc_request();
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_ASSOC && same(last.assoc.bssid, ap1, 6));
    CHECK(last.assoc.ies_len == sizeof(rsn) && same(last.ies, rsn, sizeof(rsn)));
    CHECK(last.assoc.control_port && !last.assoc.mfp && !last.assoc.reassoc);
    CHECK(last.assoc.n_pairwise == 1 && last.assoc.pairwise[0] == WLAN_CIPHER_SUITE_CCMP);
    CHECK(last.assoc.group == WLAN_CIPHER_SUITE_CCMP);
    CHECK(last.assoc.n_akm == 1 && last.assoc.akm[0] == WLAN_AKM_SUITE_PSK);
    CHECK(last.assoc.wpa_versions == 2);

    clear_events();
    len = assoc_resp(frame, 0);
    event(WLAN_EVENT_ASSOCIATE, ap1, frame, len, 0, 0, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_ASSOCIATE) != 0);
    CHECK(has_bytes(tb[NL80211_ATTR_FRAME], frame, len));
    CHECK(has_bytes(tb[NL80211_ATTR_REQ_IE], rsn, sizeof(rsn)));
    CHECK(find_event(GROUP_MLME, NL80211_CMD_CONNECT) != 0);
    CHECK(has_bytes(tb[NL80211_ATTR_MAC], ap1, 6));
    CHECK(u16_of(tb[NL80211_ATTR_STATUS_CODE]) == 0);
    CHECK(has_bytes(tb[NL80211_ATTR_REQ_IE], rsn, sizeof(rsn)));
    CHECK(has_bytes(tb[NL80211_ATTR_RESP_IE], frame + 30, 6));
    CHECK(tb[NL80211_ATTR_TIMED_OUT] == 0);
    set_associated(ap1, WLAN_STA_ASSOCIATED);

    /* The pairwise key, nested, with the AP's address. */
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, tk, 16);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);
    rq_u8(NL80211_KEY_IDX, 0);
    rq_end(nest);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_ADD_KEY && last.key.flags == WLAN_KEY_PAIRWISE);
    CHECK(same(last.key.peer, ap1, 6) && last.key.index == 0 && last.key.len == 16);
    CHECK(last.key.cipher == WLAN_CIPHER_SUITE_CCMP && same(last.key.data, tk, 16));
    CHECK(last.key.hw_slot == -1 && last.key.rx_pn[0] == 0 && last.key.tx_pn == 0);

    /* The group key with its receive counter, then made the default. */
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, tk, 16);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);
    rq_put(NL80211_KEY_SEQ, rsc, 6);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_end(nest);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_ADD_KEY && last.key.flags == 0 && same(last.key.peer, bcast, 6));
    CHECK(last.key.index == 1 && last.key.rx_pn[0] == 0x060504030201ull &&
          last.key.rx_pn[WLAN_NON_QOS_TID] == 0x060504030201ull);
    rq_begin(NL80211_CMD_SET_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_flag(NL80211_KEY_DEFAULT);
    types = rq_nest(NL80211_KEY_DEFAULT_TYPES);
    rq_flag(NL80211_KEY_DEFAULT_TYPE_MULTICAST);
    rq_end(types);
    rq_end(nest);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_DEFAULT_KEY && last.index == 1 && last.flag == 0);

    /* The port opens. */
    rq_begin(NL80211_CMD_SET_STATION, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    flags[0] = flags[1] = 1u << NL80211_STA_FLAG_AUTHORIZED;
    rq_put(NL80211_ATTR_STA_FLAGS2, flags, 8);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_AUTHORIZE && last.flag == 1 && same(last.addr, ap1, 6));
    status_now.state = WLAN_STA_AUTHORIZED;

    /* signal_poll: GET_STATION, then GET_SURVEY for the noise. */
    now += 2500;
    status_now.last_rx = now - 20;
    rq_begin(NL80211_CMD_GET_STATION, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 1 && parse(&replies[0]) == NL80211_CMD_NEW_STATION);
    CHECK(has_bytes(tb[NL80211_ATTR_MAC], ap1, 6));
    parse_nest(tb[NL80211_ATTR_STA_INFO], t, NL80211_STA_INFO_RX_DROP_MISC);
    CHECK(t[NL80211_STA_INFO_SIGNAL] && (int8_t)nla_get_u8(t[NL80211_STA_INFO_SIGNAL]) == -40);
    CHECK(t[NL80211_STA_INFO_SIGNAL_AVG] != 0);
    CHECK(u32_of(t[NL80211_STA_INFO_RX_BYTES]) == 1000 &&
          u32_of(t[NL80211_STA_INFO_TX_BYTES]) == 2000);
    CHECK(u32_of(t[NL80211_STA_INFO_INACTIVE_TIME]) == 20);
    CHECK(u32_of(t[NL80211_STA_INFO_CONNECTED_TIME]) == 2);
    CHECK(u32_of(t[NL80211_STA_INFO_BEACON_LOSS]) == 3);
    CHECK(t[NL80211_STA_INFO_BSS_PARAM] != 0);
    CHECK(t[NL80211_STA_INFO_STA_FLAGS] && nla_len(t[NL80211_STA_INFO_STA_FLAGS]) == 8 &&
          (get32(nla_data(t[NL80211_STA_INFO_STA_FLAGS]) + 4) & 1u << NL80211_STA_FLAG_AUTHORIZED));
    rq_begin(NL80211_CMD_GET_STATION, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap2, 6);
    CHECK(rq_send() == -ENOENT);
    rq_begin(NL80211_CMD_GET_STATION, 1);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0 && n_replies == 2 && parse(&replies[0]) == NL80211_CMD_NEW_STATION);
    rq_begin(NL80211_CMD_GET_SURVEY, 1);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0 && n_replies == 2);
    CHECK(parse(&replies[0]) == NL80211_CMD_NEW_SURVEY_RESULTS);
    parse_nest(tb[NL80211_ATTR_SURVEY_INFO], s, NL80211_SURVEY_INFO_IN_USE);
    CHECK(u32_of(s[NL80211_SURVEY_INFO_FREQUENCY]) == 2437);
    CHECK(s[NL80211_SURVEY_INFO_NOISE] && (int8_t)nla_get_u8(s[NL80211_SURVEY_INFO_NOISE]) == -95);
    CHECK(is_flag(s[NL80211_SURVEY_INFO_IN_USE]));

    /* The AP deauthenticates: the frame, then DISCONNECT by the AP. */
    clear_events();
    len = mgmt(frame, 0xc0, ap1, deauth_7, 2);
    event(WLAN_EVENT_DEAUTHENTICATE, ap1, frame, len, 0, 7, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_DEAUTHENTICATE) != 0);
    CHECK(has_bytes(tb[NL80211_ATTR_FRAME], frame, len));
    CHECK(find_event(GROUP_MLME, NL80211_CMD_DISCONNECT) != 0);
    CHECK(u16_of(tb[NL80211_ATTR_REASON_CODE]) == 7);
    CHECK(is_flag(tb[NL80211_ATTR_DISCONNECTED_BY_AP]));
    CHECK(!stations[0]->connected);
    k_memset(&status_now, 0, sizeof(status_now));
    rq_begin(NL80211_CMD_GET_SURVEY, 1);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0 && n_replies == 1);
}

/** DEAUTHENTICATE and DISASSOCIATE as wpa_supplicant sends them, and the
 * station's timeouts. */
static void leave_and_timeouts(void)
{
    rq_begin(NL80211_CMD_DEAUTHENTICATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u16(NL80211_ATTR_REASON_CODE, 3);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_DEAUTH && last.reason == 3 && !last.local && same(last.addr, ap1, 6));
    rq_begin(NL80211_CMD_DEAUTHENTICATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u16(NL80211_ATTR_REASON_CODE, 3);
    rq_flag(NL80211_ATTR_LOCAL_STATE_CHANGE);
    next_rc = -ENOTCONN;
    CHECK(rq_send() == -ENOTCONN);
    CHECK(last.kind == CALL_DEAUTH && last.local);
    rq_begin(NL80211_CMD_DISASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u16(NL80211_ATTR_REASON_CODE, 8);
    CHECK(rq_send() == 0 && last.kind == CALL_DISASSOC && last.reason == 8);
    rq_begin(NL80211_CMD_DEAUTHENTICATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u16(NL80211_ATTR_REASON_CODE, 0);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_DEAUTHENTICATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u16(NL80211_ATTR_REASON_CODE, 3);
    CHECK(rq_send() == -EINVAL);

    clear_events();
    event(WLAN_EVENT_AUTHENTICATE, ap1, 0, 0, 0, 0, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_AUTHENTICATE) != 0);
    CHECK(is_flag(tb[NL80211_ATTR_TIMED_OUT]) && has_bytes(tb[NL80211_ATTR_MAC], ap1, 6));
    CHECK(tb[NL80211_ATTR_FRAME] == 0 && n_events == 1);
    clear_events();
    event(WLAN_EVENT_ASSOCIATE, ap1, 0, 0, 0, 0, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_ASSOCIATE) != 0);
    CHECK(is_flag(tb[NL80211_ATTR_TIMED_OUT]));
    CHECK(find_event(GROUP_MLME, NL80211_CMD_CONNECT) != 0);
    CHECK(u16_of(tb[NL80211_ATTR_STATUS_CODE]) == WLAN_STATUS_UNSPECIFIED_FAILURE);
    CHECK(is_flag(tb[NL80211_ATTR_TIMED_OUT]));
    CHECK(u32_of(tb[NL80211_ATTR_TIMEOUT_REASON]) == NL80211_TIMEOUT_ASSOC);
    CHECK(!stations[0]->connected);
}

/** The refusals of AUTHENTICATE and ASSOCIATE. */
static void auth_assoc_refused(void)
{
    uint32_t wep = 0x000fac05u;

    n_calls = 0;
    auth_request(NL80211_AUTHTYPE_OPEN_SYSTEM + 1); /* Shared Key */
    CHECK(rq_send() == -EOPNOTSUPP);
    auth_request(NL80211_AUTHTYPE_SAE);
    CHECK(rq_send() == -EINVAL);
    auth_request(NL80211_AUTHTYPE_OPEN_SYSTEM);
    rq_put(NL80211_ATTR_AUTH_DATA, "\1\0\0\0", 4);
    CHECK(rq_send() == -EINVAL);
    auth_request(NL80211_AUTHTYPE_OPEN_SYSTEM);
    rq_flag(NL80211_ATTR_LOCAL_STATE_CHANGE);
    CHECK(rq_send() == -EOPNOTSUPP);
    auth_request(12);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_AUTHENTICATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2484);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_u32(NL80211_ATTR_AUTH_TYPE, 0);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_AUTHENTICATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 5); /* the policy wants six bytes */
    CHECK(rq_send() == -EINVAL);

    assoc_request();
    rq_u32(NL80211_ATTR_USE_MFP + 0, 0); /* again: the parser refuses repeats */
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_u32(NL80211_ATTR_USE_MFP, NL80211_MFP_OPTIONAL);
    CHECK(rq_send() == -EOPNOTSUPP);
    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_put(NL80211_ATTR_CIPHER_SUITES_PAIRWISE, &wep, 4);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_u16(NL80211_ATTR_CONTROL_PORT_ETHERTYPE, 0x0800);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_put(NL80211_ATTR_IE, req + 1000, NL80211_MAX_IE + 1);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    CHECK(rq_send() == -EINVAL);
    CHECK(n_calls == 0);

    /* Reassociation names the AP it leaves. */
    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_put(NL80211_ATTR_PREV_BSSID, ap2, 6);
    rq_flag(NL80211_ATTR_DISABLE_HT);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_ASSOC && last.assoc.reassoc && same(last.assoc.prev_bssid, ap2, 6));
    CHECK(last.assoc.disable_ht && !last.assoc.control_port && last.assoc.ies_len == 0);
}

/** WPA3-SAE: the commit carried in AUTH_DATA, association with protection
 * required, the integrity key, and an unprotected deauthentication. */
static void join_sae(void)
{
    static const uint8_t commit[8] = {1, 0, 0, 0, 19, 0, 0xaa, 0xbb};
    static const uint8_t igtk[16] = {9};
    static const uint8_t deauth_6[2] = {6, 0};
    uint32_t akm = WLAN_AKM_SUITE_SAE;
    uint8_t frame[64];
    unsigned nest, len;

    auth_request(NL80211_AUTHTYPE_SAE);
    rq_put(NL80211_ATTR_AUTH_DATA, commit, sizeof(commit));
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_AUTH && last.auth.algorithm == WLAN_AUTH_SAE);
    CHECK(last.auth.data_len == sizeof(commit) && same(last.data, commit, sizeof(commit)));

    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_put(NL80211_ATTR_AKM_SUITES, &akm, 4);
    rq_u32(NL80211_ATTR_WPA_VERSIONS, 2 | 4);
    rq_u32(NL80211_ATTR_USE_MFP, NL80211_MFP_REQUIRED);
    rq_flag(NL80211_ATTR_CONTROL_PORT);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_ASSOC && last.assoc.mfp && last.assoc.akm[0] == WLAN_AKM_SUITE_SAE);

    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, igtk, 16);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_AES_CMAC);
    rq_put(NL80211_KEY_SEQ, "\5\0\0\0\0\0", 6);
    rq_u8(NL80211_KEY_IDX, 4);
    rq_end(nest);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_ADD_KEY && last.key.index == 4 && last.key.rx_pn[0] == 5);
    CHECK(last.key.cipher == WLAN_CIPHER_SUITE_AES_CMAC && same(last.key.peer, bcast, 6));
    rq_begin(NL80211_CMD_SET_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u8(NL80211_KEY_IDX, 4);
    rq_flag(NL80211_KEY_DEFAULT_MGMT);
    rq_end(nest);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_DEFAULT_KEY && last.index == 4 && last.flag == 1);

    clear_events();
    len = mgmt(frame, 0xc0, ap1, deauth_6, 2);
    event(WLAN_EVENT_UNPROT_DEAUTHENTICATE, ap1, frame, len, 0, 6, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_UNPROT_DEAUTHENTICATE) != 0);
    CHECK(has_bytes(tb[NL80211_ATTR_FRAME], frame, len));
    CHECK(n_events == 1);
    clear_events();
    event(WLAN_EVENT_UNPROT_DISASSOCIATE, ap1, frame, len, 0, 6, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_UNPROT_DISASSOCIATE) != 0);
}

/** Key requests refused or passed through, and the older attributes. */
static void keys(void)
{
    static const uint8_t k32[32] = {1};
    unsigned nest;
    uint32_t flags[2];

    n_calls = 0;
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, k32, 32);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, k32, 16);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);
    rq_u8(NL80211_KEY_IDX, 2);
    rq_end(nest);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    CHECK(rq_send() == -EINVAL); /* a pairwise key of index 2 */
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, k32, 16);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_AES_CMAC);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, k32, 16);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);
    rq_put(NL80211_KEY_SEQ, k32, 7);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, k32, 16);
    rq_u32(NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);
    rq_u8(NL80211_KEY_IDX, 0);
    rq_u8(NL80211_KEY_MODE, 1);
    rq_end(nest);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    CHECK(rq_send() == -EOPNOTSUPP);
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_put(NL80211_KEY_DATA, k32, 13); /* WEP-104 */
    rq_u32(NL80211_KEY_CIPHER, 0x000fac05u);
    rq_u8(NL80211_KEY_IDX, 0);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    CHECK(n_calls == 0);

    /* TKIP's 32 bytes, in the older form of the top level. */
    rq_begin(NL80211_CMD_NEW_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_KEY_DATA, k32, 32);
    rq_u32(NL80211_ATTR_KEY_CIPHER, WLAN_CIPHER_SUITE_TKIP);
    rq_u8(NL80211_ATTR_KEY_IDX, 0);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_ADD_KEY && last.key.len == 32 && last.key.flags == WLAN_KEY_PAIRWISE);

    /* DEL_KEY as wpa_supplicant clears keys: -ENOENT passes through. */
    rq_begin(NL80211_CMD_DEL_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u8(NL80211_KEY_IDX, 2);
    rq_end(nest);
    next_rc = -ENOENT;
    CHECK(rq_send() == -ENOENT);
    CHECK(last.kind == CALL_DEL_KEY && last.index == 2 && same(last.addr, bcast, 6));
    rq_begin(NL80211_CMD_DEL_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u8(NL80211_KEY_IDX, 0);
    rq_end(nest);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    CHECK(rq_send() == 0 && last.kind == CALL_DEL_KEY && same(last.addr, ap1, 6));

    /* A unicast default changes nothing; a default without one is refused. */
    n_calls = 0;
    rq_begin(NL80211_CMD_SET_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u8(NL80211_KEY_IDX, 0);
    rq_flag(NL80211_KEY_DEFAULT);
    flags[0] = rq_nest(NL80211_KEY_DEFAULT_TYPES);
    rq_flag(NL80211_KEY_DEFAULT_TYPE_UNICAST);
    rq_end(flags[0]);
    rq_end(nest);
    CHECK(rq_send() == 0 && n_calls == 0);
    rq_begin(NL80211_CMD_SET_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_SET_KEY, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    nest = rq_nest(NL80211_ATTR_KEY);
    rq_u8(NL80211_KEY_IDX, 1);
    rq_flag(NL80211_KEY_DEFAULT_MGMT);
    rq_end(nest);
    CHECK(rq_send() == -EINVAL);

    /* SET_STATION may change AUTHORIZED alone. */
    rq_begin(NL80211_CMD_SET_STATION, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    flags[0] = 1u << NL80211_STA_FLAG_WME;
    flags[1] = 0;
    rq_put(NL80211_ATTR_STA_FLAGS2, flags, 8);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_SET_STATION, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_MAC, ap1, 6);
    flags[0] = 1u << NL80211_STA_FLAG_AUTHORIZED;
    flags[1] = 0;
    rq_put(NL80211_ATTR_STA_FLAGS2, flags, 8);
    CHECK(rq_send() == 0 && last.kind == CALL_AUTHORIZE && last.flag == 0);
    CHECK(n_calls == 1);
}

/** CONNECT as iw connect sends it: a scan for the SSID, the strongest BSS,
 * authentication and association, then DISCONNECT. */
static void connect_row(void)
{
    static const uint8_t auth_ok[6] = {0, 0, 2, 0, 0, 0};
    static const uint8_t auth_no[6] = {0, 0, 2, 0, 17, 0};
    static const uint8_t deauth_3[2] = {3, 0};
    uint8_t frame[64];
    unsigned len;
    uint64_t cookie;

    wlan_bss_flush(&table);
    clear_events();
    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_put(NL80211_ATTR_IE, rsn, sizeof(rsn));
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_SCAN && last.scan.n_ssids == 1 && last.scan.ssids[0].len == 3);
    cookie = last.scan.cookie;
    hear(ap2, "lab", 1, -60);
    hear(ap1, "lab", 6, -40);
    hear(ap3, "other", 6, -20);
    scan_ev(WLAN_EVENT_SCAN_DONE, cookie);
    CHECK(find_event(GROUP_SCAN, NL80211_CMD_NEW_SCAN_RESULTS) != 0);
    check_scan_request("lab");
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(last.kind == CALL_AUTH && same(last.auth.bssid, ap1, 6));
    CHECK(last.auth.algorithm == WLAN_AUTH_OPEN && last.auth.frequency == 2437);

    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    CHECK(rq_send() == -EALREADY);

    len = mgmt(frame, 0xb0, ap1, auth_ok, 6);
    event(WLAN_EVENT_AUTHENTICATE, ap1, frame, len, 0, 0, 0);
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(last.kind == CALL_ASSOC && same(last.assoc.bssid, ap1, 6));
    CHECK(last.assoc.ies_len == sizeof(rsn) && same(last.ies, rsn, sizeof(rsn)));
    clear_events();
    len = assoc_resp(frame, 0);
    event(WLAN_EVENT_ASSOCIATE, ap1, frame, len, 0, 0, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_CONNECT) != 0);
    CHECK(u16_of(tb[NL80211_ATTR_STATUS_CODE]) == 0 && has_bytes(tb[NL80211_ATTR_MAC], ap1, 6));
    CHECK(has_bytes(tb[NL80211_ATTR_REQ_IE], rsn, sizeof(rsn)));
    CHECK(stations[0]->connected && stations[0]->conn == 0);

    rq_begin(NL80211_CMD_DISCONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u16(NL80211_ATTR_REASON_CODE, 3);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_DEAUTH && same(last.addr, ap1, 6) && last.reason == 3 && !last.local);
    clear_events();
    len = mgmt(frame, 0xc0, ap1, deauth_3, 2);
    event(WLAN_EVENT_DEAUTHENTICATE, ap1, frame, len, 0, 3, 1);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_DISCONNECT) != 0);
    CHECK(tb[NL80211_ATTR_DISCONNECTED_BY_AP] == 0 && u16_of(tb[NL80211_ATTR_REASON_CODE]) == 3);
    rq_begin(NL80211_CMD_DISCONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == -ENOTCONN);

    /* The AP refuses authentication: CONNECT ends with its status. */
    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_put(NL80211_ATTR_MAC, ap2, 6);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_AUTH && same(last.auth.bssid, ap2, 6) && last.auth.frequency == 2412);
    clear_events();
    len = mgmt(frame, 0xb0, ap2, auth_no, 6);
    event(WLAN_EVENT_AUTHENTICATE, ap2, frame, len, 17, 0, 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_CONNECT) != 0);
    CHECK(u16_of(tb[NL80211_ATTR_STATUS_CODE]) == 17 && has_bytes(tb[NL80211_ATTR_MAC], ap2, 6));
    CHECK(tb[NL80211_ATTR_REQ_IE] == 0 && stations[0]->conn == 0);

    /* An SSID nobody has: one scan, then CONNECT ends for want of it. */
    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "gone", 4);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2462);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_SCAN && last.scan.n_frequencies == 1 &&
          last.scan.frequencies[0] == 2462);
    clear_events();
    scan_ev(WLAN_EVENT_SCAN_DONE, last.scan.cookie);
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_CONNECT) != 0);
    CHECK(u16_of(tb[NL80211_ATTR_STATUS_CODE]) == WLAN_STATUS_UNSPECIFIED_FAILURE);
    CHECK(u32_of(tb[NL80211_ATTR_TIMEOUT_REASON]) == NL80211_TIMEOUT_SCAN);
    CHECK(tb[NL80211_ATTR_MAC] == 0 && stations[0]->conn == 0);

    /* DISCONNECT while authenticating ends the CONNECT quietly. */
    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    CHECK(rq_send() == 0 && last.kind == CALL_AUTH);
    clear_events();
    rq_begin(NL80211_CMD_DISCONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_DEAUTH && same(last.addr, ap1, 6) && last.reason == 3);
    CHECK(stations[0]->conn == 0 && n_events == 0);

    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_u32(NL80211_ATTR_AUTH_TYPE, NL80211_AUTHTYPE_SAE);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "", 0);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_flag(NL80211_ATTR_PRIVACY);
    CHECK(rq_send() == -EOPNOTSUPP); /* WEP */
    CHECK(stations[0]->conn == 0);
}

/** An action frame to the station: SA Query and WNM as wpa_supplicant
 * registers for them. */
static unsigned action(uint8_t *frame, unsigned category, unsigned code)
{
    uint8_t body[4];

    body[0] = (uint8_t)category;
    body[1] = (uint8_t)code;
    body[2] = 0x12;
    body[3] = 0x34;
    return mgmt(frame, 0xd0, ap1, body, 4);
}

static int register_match(uint32_t port, unsigned type, const char *match, unsigned len)
{
    rq_begin(NL80211_CMD_REGISTER_FRAME, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u16(NL80211_ATTR_FRAME_TYPE, type);
    rq_put(NL80211_ATTR_FRAME_MATCH, match, len);
    return rq_send_from(port);
}

/** REGISTER_FRAME, frames delivered to their registrations, the release
 * of a closed socket's, FRAME sent and its status. */
static void frames(void)
{
    uint8_t frame[64];
    unsigned len;
    uint64_t cookie;
    struct wlan_event ev;

    CHECK(register_match(PORT, 0xd0, "\x08\x01", 2) == 0);
    CHECK(register_match(PORT2, 0xd0, "\x08\x01", 2) == -EALREADY);
    CHECK(register_match(PORT2, 0xd0, "\x08", 1) == -EALREADY);
    CHECK(register_match(PORT2, 0xd0, "\x0a\x07", 2) == 0);
    CHECK(register_match(PORT2, 0x40, "", 0) == -EINVAL); /* probe requests */
    CHECK(register_match(PORT2, 0xd8, "\x04", 1) == -EINVAL);
    rq_begin(NL80211_CMD_REGISTER_FRAME, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == -EINVAL);
    CHECK(stations[0]->n_regs == 2);

    clear_events();
    len = action(frame, 8, 1);
    event(WLAN_EVENT_FRAME, ap1, frame, len, 0, 0, 0);
    CHECK(n_unicasts == 1 && unicasts[0].to == PORT);
    CHECK(parse(&unicasts[0]) == NL80211_CMD_FRAME);
    CHECK(has_bytes(tb[NL80211_ATTR_FRAME], frame, len));
    CHECK(u32_of(tb[NL80211_ATTR_WIPHY_FREQ]) == 2437 && u32_of(tb[NL80211_ATTR_IFINDEX]) == 2);
    clear_events();
    len = action(frame, 0x0a, 7);
    event(WLAN_EVENT_FRAME, ap1, frame, len, 0, 0, 0);
    CHECK(n_unicasts == 1 && unicasts[0].to == PORT2);
    clear_events();
    len = action(frame, 4, 10);
    event(WLAN_EVENT_FRAME, ap1, frame, len, 0, 0, 0);
    CHECK(n_unicasts == 0 && n_events == 0);

    /* The socket of PORT closes; its match is free again. */
    genl_release(PORT);
    CHECK(stations[0]->n_regs == 1);
    clear_events();
    len = action(frame, 8, 1);
    event(WLAN_EVENT_FRAME, ap1, frame, len, 0, 0, 0);
    CHECK(n_unicasts == 0);
    CHECK(register_match(PORT, 0xd0, "\x08\x01", 2) == 0);
    genl_release(PORT);
    genl_release(PORT2);
    CHECK(stations[0]->n_regs == 0);

    /* A socket gone without its release: the delivery finds it so. */
    CHECK(register_match(DEAD_PORT, 0xd0, "\x08", 1) == 0);
    clear_events();
    event(WLAN_EVENT_FRAME, ap1, frame, len, 0, 0, 0);
    CHECK(n_unicasts == 0 && stations[0]->n_regs == 0);

    /* FRAME: an SA Query request on channel 6, then its status. */
    len = action(frame, 8, 0);
    rq_begin(NL80211_CMD_FRAME, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_FRAME, frame, len);
    CHECK(rq_send() == 0);
    CHECK(last.kind == CALL_TX && last.index == len && same(last.data, frame, len));
    CHECK(last.frequency == 2437 && (last.cookie & NL80211_COOKIE) &&
          !(last.cookie & NL80211_COOKIE_NOACK));
    cookie = last.cookie;
    CHECK(n_replies == 1 && parse(&replies[0]) == NL80211_CMD_FRAME);
    CHECK(tb[NL80211_ATTR_COOKIE] && nla_get_u64(tb[NL80211_ATTR_COOKIE]) == cookie);
    clear_events();
    k_memset(&ev, 0, sizeof(ev));
    ev.type = WLAN_EVENT_TX_STATUS;
    ev.cookie = cookie;
    ev.frame = frame;
    ev.frame_len = len;
    ev.acked = 1;
    wlan_event(WLAN1, &ev);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_FRAME_TX_STATUS) != 0);
    CHECK(tb[NL80211_ATTR_COOKIE] && nla_get_u64(tb[NL80211_ATTR_COOKIE]) == cookie);
    CHECK(is_flag(tb[NL80211_ATTR_ACK]) && has_bytes(tb[NL80211_ATTR_FRAME], frame, len));

    /* No acknowledgement asked for: no cookie in the answer, no status. */
    rq_begin(NL80211_CMD_FRAME, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_FRAME, frame, len);
    rq_flag(NL80211_ATTR_DONT_WAIT_FOR_ACK);
    CHECK(rq_send() == 0 && n_replies == 0);
    CHECK(last.frequency == 0 && (last.cookie & NL80211_COOKIE_NOACK));
    clear_events();
    ev.cookie = last.cookie;
    wlan_event(WLAN1, &ev);
    ev.cookie = 77; /* the data path's */
    wlan_event(WLAN1, &ev);
    CHECK(n_events == 0);

    len = mgmt(frame, 0xc0, ap1, (const uint8_t *)"\3\0", 2);
    rq_begin(NL80211_CMD_FRAME, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_FRAME, frame, len);
    CHECK(rq_send() == -EINVAL);
    rq_begin(NL80211_CMD_FRAME, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_put(NL80211_ATTR_FRAME, frame, 20);
    CHECK(rq_send() == -EINVAL);
}

/** GET_REG, REG_CHANGE, MICHAEL_MIC_FAILURE and power save. */
static void reg_mic_power(void)
{
    const struct nlattr *rules[3], *r[7];
    struct wlan_event ev;

    rq_begin(NL80211_CMD_GET_REG, 0);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 1 && parse(&replies[0]) == NL80211_CMD_GET_REG);
    CHECK(has_bytes(tb[NL80211_ATTR_REG_ALPHA2], "00", 3));
    parse_nest(tb[NL80211_ATTR_REG_RULES], rules, 2);
    parse_nest(rules[0], r, 6);
    CHECK(u32_of(r[NL80211_ATTR_FREQ_RANGE_START]) == 2402000);
    CHECK(u32_of(r[NL80211_ATTR_FREQ_RANGE_END]) == 2472000);
    CHECK(u32_of(r[NL80211_ATTR_REG_RULE_FLAGS]) == 0);
    parse_nest(rules[1], r, 6);
    CHECK(u32_of(r[NL80211_ATTR_FREQ_RANGE_END]) == 2482000);
    CHECK(u32_of(r[NL80211_ATTR_REG_RULE_FLAGS]) & NL80211_RRF_NO_IR);
    CHECK(u32_of(r[NL80211_ATTR_POWER_RULE_MAX_EIRP]) == 2000);
    rq_begin(NL80211_CMD_GET_REG, 0);
    rq_u32(NL80211_ATTR_WIPHY, 5);
    CHECK(rq_send() == -ENODEV);

    clear_events();
    k_memset(&ev, 0, sizeof(ev));
    ev.type = WLAN_EVENT_REG_CHANGE;
    wlan_event(WLAN1, &ev);
    CHECK(find_event(GROUP_REG, NL80211_CMD_REG_CHANGE) != 0);
    CHECK(tb[NL80211_ATTR_REG_TYPE] &&
          nla_get_u8(tb[NL80211_ATTR_REG_TYPE]) == NL80211_REGDOM_TYPE_WORLD);
    CHECK(has_bytes(tb[NL80211_ATTR_REG_ALPHA2], "00", 3));
    radio.country[0] = 'D';
    radio.country[1] = 'E';
    clear_events();
    wlan_event(WLAN1, &ev);
    CHECK(find_event(GROUP_REG, NL80211_CMD_REG_CHANGE) != 0);
    CHECK(nla_get_u8(tb[NL80211_ATTR_REG_TYPE]) == NL80211_REGDOM_TYPE_COUNTRY);
    CHECK(has_bytes(tb[NL80211_ATTR_REG_ALPHA2], "DE", 3));
    radio.country[0] = radio.country[1] = '0';

    clear_events();
    k_memset(&ev, 0, sizeof(ev));
    ev.type = WLAN_EVENT_MIC_FAILURE;
    k_memcpy(ev.peer, ap1, 6);
    ev.pairwise = 1;
    wlan_event(WLAN1, &ev);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_MICHAEL_MIC_FAILURE) != 0);
    CHECK(u32_of(tb[NL80211_ATTR_KEY_TYPE]) == NL80211_KEYTYPE_PAIRWISE);
    CHECK(tb[NL80211_ATTR_KEY_IDX] && nla_get_u8(tb[NL80211_ATTR_KEY_IDX]) == 0);
    CHECK(has_bytes(tb[NL80211_ATTR_MAC], ap1, 6));

    rq_begin(NL80211_CMD_GET_POWER_SAVE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    CHECK(rq_send() == 0 && n_replies == 1 && parse(&replies[0]) == NL80211_CMD_GET_POWER_SAVE);
    CHECK(u32_of(tb[NL80211_ATTR_PS_STATE]) == NL80211_PS_DISABLED);
    rq_begin(NL80211_CMD_SET_POWER_SAVE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u32(NL80211_ATTR_PS_STATE, NL80211_PS_ENABLED);
    CHECK(rq_send() == -EOPNOTSUPP);
    rq_begin(NL80211_CMD_SET_POWER_SAVE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 2);
    rq_u32(NL80211_ATTR_PS_STATE, NL80211_PS_DISABLED);
    CHECK(rq_send() == 0);
}

/** A second interface: both listed, a request to it ended by its
 * detachment, and the attachment's refusals. */
static void attachment(void)
{
    uint32_t before = generation;
    struct netdev extra[4];
    unsigned i;

    k_memset(&radio2, 0, sizeof(radio2));
    radio2.index = 1;
    radio2.country[0] = radio2.country[1] = '0';
    k_memset(&nd2, 0, sizeof(nd2));
    k_memcpy(nd2.name, "wlan1", 6);
    nd2.index = 3;
    wlan_bss_init(&table2);
    CHECK(nl80211_attach(WLAN2, &radio2, &band, &nd2, &table2, &station_ops) == 0);
    CHECK(generation == before + 1);
    CHECK(nl80211_attach(WLAN2, &radio2, &band, &nd2, &table2, &station_ops) == -EEXIST);
    CHECK(nl80211_attach(0, &radio2, &band, &nd2, &table2, &station_ops) == -EINVAL);

    rq_begin(NL80211_CMD_GET_INTERFACE, 1);
    CHECK(rq_send() == 0 && n_replies == 3);
    rq_begin(NL80211_CMD_GET_INTERFACE, 1);
    rq_u32(NL80211_ATTR_WIPHY, 1);
    CHECK(rq_send() == 0 && n_replies == 2 && parse(&replies[0]) == NL80211_CMD_NEW_INTERFACE);
    CHECK(has_bytes(tb[NL80211_ATTR_IFNAME], "wlan1", 6));
    CHECK(tb[NL80211_ATTR_WDEV] && nla_get_u64(tb[NL80211_ATTR_WDEV]) == ((uint64_t)1 << 32 | 1));
    rq_begin(NL80211_CMD_GET_WIPHY, 1);
    rq_flag(NL80211_ATTR_SPLIT_WIPHY_DUMP);
    CHECK(rq_send() == 0 && n_replies == 9);

    /* The station goes while a scan request waits for the worker. */
    n_calls = 0;
    detach_in_wait = 1;
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 3);
    CHECK(rq_send() == -ENODEV);
    CHECK(n_calls == 0 && wlan_work_next_due() == 0);
    rq_begin(NL80211_CMD_GET_INTERFACE, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 3);
    CHECK(rq_send() == -ENODEV);
    nl80211_detach(WLAN2); /* again: nothing */

    k_memset(extra, 0, sizeof(extra));
    for (i = 0; i < MAX_STATIONS - 1; ++i) {
        extra[i].index = (int)(10 + i);
        CHECK(nl80211_attach((struct wlan *)(void *)&extra[i], &radio2, &band, &extra[i], &table2,
                             &station_ops) == 0);
    }
    CHECK(nl80211_attach((struct wlan *)(void *)&extra[3], &radio2, &band, &extra[3], &table2,
                         &station_ops) == -ENOSPC);
    for (i = 0; i < MAX_STATIONS - 1; ++i) {
        nl80211_detach((struct wlan *)(void *)&extra[i]);
    }
    wlan_bss_destroy(&table2);
}

static struct {
    int connects, disconnects;
    struct nl80211_connect req;
    struct wlan_ssid ssid;
    uint8_t bssid[6];
    int any_bssid;
    unsigned reason, ies_len;
} full;

static int fm_connect(struct wlan *wlan, const struct nl80211_connect *req)
{
    CHECK(wlan == WLAN3);
    ++full.connects;
    full.req = *req;
    full.ssid = req->ssid;
    full.any_bssid = req->bssid == 0;
    if (req->bssid) {
        k_memcpy(full.bssid, req->bssid, 6);
    }
    full.ies_len = req->assoc->ies_len;
    return 0;
}

static int fm_disconnect(struct wlan *wlan, unsigned reason)
{
    CHECK(wlan == WLAN3);
    ++full.disconnects;
    full.reason = reason;
    return 0;
}

/** A station that joins by itself, as a fullMAC chip does: CONNECT and
 * DISCONNECT go to its own calls, and its ASSOCIATE event ends the CONNECT. */
static void fullmac(void)
{
    static struct nl80211_ops ops;
    static struct wlan_radio radio3;
    static struct netdev nd3;
    static struct wlan_bss_table table3;
    struct wlan_event ev;
    uint8_t frame[64];
    unsigned len;

    ops = station_ops;
    ops.connect = fm_connect;
    ops.disconnect = fm_disconnect;
    k_memset(&radio3, 0, sizeof(radio3));
    radio3.index = 2;
    radio3.country[0] = radio3.country[1] = '0';
    k_memset(&nd3, 0, sizeof(nd3));
    k_memcpy(nd3.name, "wlan2", 6);
    nd3.index = 4;
    wlan_bss_init(&table3);
    CHECK(nl80211_attach(WLAN3, &radio3, &band, &nd3, &table3, &ops) == 0);

    n_calls = 0;
    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 4);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, 2437);
    rq_put(NL80211_ATTR_IE, rsn, sizeof(rsn));
    CHECK(rq_send() == 0);
    CHECK(full.connects == 1 && n_calls == 0);
    CHECK(full.ssid.len == 3 && same(full.ssid.data, "lab", 3) && full.any_bssid);
    CHECK(full.req.frequency == 2437 && full.ies_len == sizeof(rsn));

    clear_events();
    k_memset(&ev, 0, sizeof(ev));
    ev.type = WLAN_EVENT_ASSOCIATE;
    k_memcpy(ev.peer, ap1, 6);
    len = assoc_resp(frame, 0);
    ev.frame = frame;
    ev.frame_len = len;
    wlan_event(WLAN3, &ev);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_CONNECT) != 0);
    CHECK(u32_of(tb[NL80211_ATTR_IFINDEX]) == 4 && u16_of(tb[NL80211_ATTR_STATUS_CODE]) == 0);
    CHECK(has_bytes(tb[NL80211_ATTR_REQ_IE], rsn, sizeof(rsn)));

    rq_begin(NL80211_CMD_CONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 4);
    rq_put(NL80211_ATTR_SSID, "lab", 3);
    CHECK(rq_send() == -EALREADY && full.connects == 1);
    rq_begin(NL80211_CMD_DISCONNECT, 0);
    rq_u32(NL80211_ATTR_IFINDEX, 4);
    rq_u16(NL80211_ATTR_REASON_CODE, 8);
    CHECK(rq_send() == 0 && full.disconnects == 1 && full.reason == 8 && n_calls == 0);

    nl80211_detach(WLAN3);
    wlan_bss_destroy(&table3);
}

int main(void)
{
    setup();
    resolve();
    protocol_features();
    wiphy_split();
    wiphy_whole();
    interfaces();
    scan();
    join_wpa2();
    leave_and_timeouts();
    auth_assoc_refused();
    join_sae();
    keys();
    connect_row();
    frames();
    reg_mic_power();
    attachment();
    fullmac();
    nl80211_detach(WLAN1);
    wlan_bss_destroy(&table);
    CHECK(wlan_work_next_due() == 0);
    if (failures) {
        printf("nl80211_hosttest: %u failures\n", failures);
        return 1;
    }
    printf("nl80211_hosttest: ok\n");
    return 0;
}
