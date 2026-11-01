/**
 * Join a wsim radio to a scripted access point through the real station
 * layer: the registry, the worker, the scan and BSS table, the MLME, the
 * softMAC station, the data path and nl80211, with /dev/wsim as the medium.
 * The rows ask what wpa_supplicant asks of an open network: a scan,
 * Open System authentication, association with a control port and its
 * authorization; then an IP datagram goes each way, the interface goes down
 * and comes up again, and an rfkill block ends the association.
 *
 * host-deps: rach2/sys/network/wireless/wiphy.c
 * host-deps: rach2/sys/network/wireless/wlan.c
 * host-deps: rach2/sys/network/wireless/reg.c
 * host-deps: rach2/sys/network/wireless/sta.c
 * host-deps: rach2/sys/network/wireless/mlme.c
 * host-deps: rach2/sys/network/wireless/work.c
 * host-deps: rach2/sys/network/wireless/scan.c
 * host-deps: rach2/sys/network/wireless/bss.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/data.c
 * host-deps: rach2/sys/network/wireless/tx.c
 * host-deps: rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/wsim.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/network/socket/genetlink.c
 * host-deps: rach2/sys/rfkill.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The test includes nl80211.c to reach its family and groups. It stands in
 * for the worker thread by running the due work itself, for the network
 * stack below the interface by keeping each frame the interface delivers,
 * and for the medium by reading and writing records of /dev/wsim.
 */
#include "../../rach2/sys/network/wireless/nl80211.c"
#include "../../rach2/sys/network/wireless/wsim.h"
#include "../../rach2/sys/network/wireless/data.h"
#include "../../rach2/sys/network/wireless/sta.h"
#include "../../rach2/sys/network/socket/netlink.h"
#include "../../rach2/sys/network/net.h"
#include "../../rach2/sys/network/netconf.h"
#include "../../rach2/sys/network/arp/arp.h"
#include "../../rach2/sys/network/ip6/route6.h"
#include "../../rach2/sys/network/ip6/nd6.h"
#include "../../rach2/sys/network/ethernet/ethernet.h"
#include "../../rach2/sys/rfkill.h"
#include "../../rach2/sys/fs/fd.h"
#include "../../rach2/sys/lib/kirq.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            printf("wlan_join_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

#define PORT     0x1234u /* the socket wpa_supplicant's requests come from */
#define F_ACK    0x004u
#define F_DUMP   0x300u
#define FREQ     2437 /* the AP's channel, 6 */
#define IFINDEX  3    /* the interface index the model network table gives */
#define NLMSG_DN 3    /* NLMSG_DONE */

static const uint8_t ap[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
/* A host behind the AP. */
static const uint8_t host[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
static const uint8_t sta_mac[6] = {0x02, 0x77, 0x73, 0, 0, 0}; /* wsim0 */
static const char ssid[] = "lab";

/* ---- the machine around the code ---- */

static uint64_t now = 1000;
static uint32_t irq_flag = 1;
static int lock_depth;

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

uint32_t net_lock(void)
{
    return (uint32_t)lock_depth++;
}

void net_unlock(uint32_t flags)
{
    CHECK(lock_depth == (int)flags + 1);
    lock_depth = (int)flags;
}

int net_admin(void)
{
    return 1;
}

/** No rach2.wsim: the radio comes from an open of /dev/wsim. */
int kcmdline_value(const char *key, char *out, unsigned int cap)
{
    (void)key;
    (void)out;
    (void)cap;
    return 0;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** The worker thread's part: run what is due until nothing is left. */
static void run_worker(void)
{
    while (wlan_work_run_due(now)) {
    }
}

/** The thread exists from the first registration; the test runs its work. */
int wlan_work_start(void)
{
    return 0;
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** No reader of /dev/wsim or /dev/rfkill blocks here. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

/** A request carried to the worker, or an interface going down: run the
 * worker until it has run. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(lock_depth == 0);
    while (!cond(arg)) {
        if (!wlan_work_run_due(now)) {
            CHECK(!"a waited request never ran");
            return 0;
        }
    }
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t len)
{
    k_memcpy(dst, src, len);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t len)
{
    k_memcpy(dst, src, len);
    return 0;
}

/* ---- the network stack below the interface ---- */

static struct netdev *iface;
static uint8_t delivered[ETH_HDR_SIZE + ETH_MTU];
static unsigned delivered_len, n_delivered;

int netdev_register(struct netdev *nd)
{
    CHECK(!iface);
    iface = nd;
    nd->index = IFINDEX;
    return 0;
}

void netdev_unregister(struct netdev *nd)
{
    CHECK(nd == iface);
    iface = 0;
}

int netdev_route_at(int n, struct netdev_rt *out)
{
    (void)n;
    (void)out;
    return 0;
}

int netdev_route_del(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw)
{
    (void)nd;
    (void)dst;
    (void)mask;
    (void)gw;
    return 0;
}

/** As netconf.c's: offer the change to the driver, which may refuse it. */
int netconf_set_up(struct netdev *nd, int up)
{
    int rc;

    up = up ? 1 : 0;
    if (up != nd->up && nd->ops->set_up) {
        rc = nd->ops->set_up(nd->dev, up);
        if (rc) {
            return rc;
        }
    }
    nd->up = up;
    return 0;
}

int netconf_set_address(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    (void)nd;
    (void)ip;
    (void)mask;
    return 0;
}

void nd6_if_down(struct netdev *nd)
{
    (void)nd;
}

void route6_forget(struct netdev *nd, int proto)
{
    (void)nd;
    (void)proto;
}

int nd6_snapshot(struct nd6_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

int nd6_forget(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

void arp_forget_dev(struct netdev *nd)
{
    (void)nd;
}

int arp_snapshot(struct arp_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

int arp_forget(struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return 0;
}

/** Keep the frame the interface hands up, as Ethernet's receive would see
 * it. */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    CHECK(nd == iface && lock_depth == 1);
    if (len <= sizeof(delivered)) {
        k_memcpy(delivered, data, len);
        delivered_len = len;
    }
    ++n_delivered;
}

/* ---- generic netlink's transport ---- */

struct dgram {
    uint32_t to; /* port, or group for a multicast */
    unsigned len;
    uint8_t data[8192];
};

static struct dgram replies[8], events[16];
static unsigned n_replies, n_events, n_acks;
static int ack_error;

static void capture(struct dgram *list, unsigned *n, unsigned cap, uint32_t to, const uint8_t *data,
                    unsigned len)
{
    if (*n < cap && len <= sizeof(list[0].data)) {
        list[*n].to = to;
        list[*n].len = len;
        k_memcpy(list[*n].data, data, len);
    }
    ++*n;
}

int netlink_genl_reply(struct socket *sock, uint32_t portid, const uint8_t *data, unsigned len)
{
    (void)sock;
    capture(replies, &n_replies, 8, portid, data, len);
    return 0;
}

int netlink_genl_multicast(uint32_t group, const uint8_t *data, unsigned len)
{
    capture(events, &n_events, 16, group, data, len);
    return 0;
}

int netlink_genl_unicast(uint32_t portid, const uint8_t *data, unsigned len)
{
    (void)portid;
    (void)data;
    (void)len;
    return 0;
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

/* ---- requests, built as libnl builds them ---- */

static uint8_t req[1024];
static unsigned req_len;
static uint32_t seq;

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

static unsigned get16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16;
}

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

static void rq_u32(unsigned type, uint32_t v)
{
    uint8_t b[4];

    set32(b, v);
    rq_put(type, b, 4);
}

/** Begin nl80211's command cmd on the interface; a dump when dump is set,
 * else a request acknowledged. */
static void rq_begin(unsigned cmd, int dump)
{
    k_memset(req, 0, 20);
    req_len = 20;
    set16(req + 4, family.id);
    set16(req + 6, 1u | (dump ? F_DUMP : F_ACK));
    set32(req + 8, ++seq);
    req[16] = (uint8_t)cmd;
    req[17] = 1;
    rq_u32(NL80211_ATTR_IFINDEX, IFINDEX);
}

/** Send the request; the acknowledgement's error, or a dump's. */
static int rq_send(void)
{
    set32(req, req_len);
    n_replies = n_acks = 0;
    ack_error = 0;
    CHECK(genl_dispatch(0, PORT, req, req_len) == 0);
    CHECK(lock_depth == 0);
    if (n_acks) {
        return ack_error;
    }
    if (n_replies && n_replies <= 8 && get16(replies[n_replies - 1].data + 4) == NLMSG_DN) {
        return (int)get32(replies[n_replies - 1].data + 16);
    }
    return 0;
}

#define TB_MAX 300
static const struct nlattr *tb[TB_MAX + 1];

/** Parse message d into tb; its command, or -1. */
static int parse(const struct dgram *d)
{
    unsigned len = get32(d->data);

    k_memset(tb, 0, sizeof(tb));
    if (len < 20 || len > d->len || get16(d->data + 4) != family.id) {
        return -1;
    }
    CHECK(nla_parse(d->data + 20, len - 20, 0, TB_MAX, tb, 0) == 0);
    return d->data[16];
}

/** The event cmd in group, parsed into tb, or NULL. */
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

static int has_bytes(const struct nlattr *a, const void *data, unsigned len)
{
    return a && nla_len(a) == len && same(nla_data(a), data, len);
}

/* ---- the medium and the access point ---- */

static fd_entry_t dev;
static struct open_file dev_file;
static uint8_t rec[WLAN_RELAY_HEADER_LEN + IEEE80211_MAX_FRAME_LEN];
static unsigned ap_seq;

static void put16be(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static unsigned get16be(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

/** Fill a record header of type for a frame of len bytes. */
static void record_header(uint8_t *r, unsigned type, unsigned flags, unsigned len)
{
    k_memset(r, 0, WLAN_RELAY_HEADER_LEN);
    put16be(r + WLAN_RELAY_OFF_MAGIC, WLAN_RELAY_MAGIC >> 16);
    put16be(r + WLAN_RELAY_OFF_MAGIC + 2, WLAN_RELAY_MAGIC & 0xffff);
    r[WLAN_RELAY_OFF_VERSION] = WLAN_RELAY_VERSION;
    r[WLAN_RELAY_OFF_TYPE] = (uint8_t)type;
    put16be(r + WLAN_RELAY_OFF_HEADER_LEN, WLAN_RELAY_HEADER_LEN);
    put16be(r + WLAN_RELAY_OFF_FLAGS, flags);
    put16be(r + WLAN_RELAY_OFF_FRAME_LEN, len);
}

/**
 * Take the next frame the radio sent off /dev/wsim into f, answer its
 * status as the medium does, acknowledged unless sent without one, and
 * return its length; 0 when none waits.
 */
static unsigned take(uint8_t *f)
{
    uint8_t status[WLAN_RELAY_HEADER_LEN];
    int32_t n = wsim_read(&dev, (char *)rec, sizeof(rec));
    unsigned flags, len;

    if (n <= 0) {
        CHECK(n == -EAGAIN);
        return 0;
    }
    flags = get16be(rec + WLAN_RELAY_OFF_FLAGS);
    len = get16be(rec + WLAN_RELAY_OFF_FRAME_LEN);
    CHECK(rec[WLAN_RELAY_OFF_TYPE] == WLAN_RELAY_FRAME && (flags & WLAN_RELAY_F_TX));
    CHECK(get16be(rec + WLAN_RELAY_OFF_FREQUENCY) == FREQ);
    k_memcpy(f, rec + WLAN_RELAY_HEADER_LEN, len);
    record_header(status, WLAN_RELAY_TX_STATUS,
                  WLAN_RELAY_F_TX |
                      (flags & WLAN_RELAY_F_NO_ACK ? WLAN_RELAY_F_NO_ACK : WLAN_RELAY_F_ACKED),
                  0);
    k_memcpy(status + WLAN_RELAY_OFF_COOKIE, rec + WLAN_RELAY_OFF_COOKIE, 8);
    status[WLAN_RELAY_OFF_ATTEMPTS] = 1;
    CHECK(wsim_write(&dev, (const char *)status, WLAN_RELAY_HEADER_LEN) == WLAN_RELAY_HEADER_LEN);
    return len;
}

/** Take every frame the radio sent and keep none; how many there were. */
static unsigned drain(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN];
    unsigned n = 0;

    while (take(f)) {
        ++n;
    }
    return n;
}

/** Have the radio receive frame f from the AP at 1 Mbit/s and -40 dBm;
 * returns the write's result. */
static int32_t hear(const uint8_t *f, unsigned len)
{
    record_header(rec, WLAN_RELAY_FRAME, 0, len);
    put16be(rec + WLAN_RELAY_OFF_FREQUENCY, FREQ);
    put16be(rec + WLAN_RELAY_OFF_RATE, 10);
    put16be(rec + WLAN_RELAY_OFF_SIGNAL, (unsigned)(-40 & 0xffff));
    k_memcpy(rec + WLAN_RELAY_HEADER_LEN, f, len);
    return wsim_write(&dev, (const char *)rec, WLAN_RELAY_HEADER_LEN + len);
}

/** Write a management header of subtype fc from the AP to da. */
static unsigned mgmt(uint8_t *f, unsigned fc, const uint8_t *da)
{
    k_memset(f, 0, 24);
    f[0] = (uint8_t)fc;
    k_memcpy(f + 4, da, 6);
    k_memcpy(f + 10, ap, 6);
    k_memcpy(f + 16, ap, 6);
    set16(f + 22, (ap_seq++ & 0xfff) << 4);
    return 24;
}

/** Append the AP's SSID, rates and channel elements at f + n. */
static unsigned ap_elems(uint8_t *f, unsigned n)
{
    static const uint8_t rates[6] = {1, 4, 0x82, 0x84, 0x8b, 0x96};
    static const uint8_t ds[3] = {3, 1, 6};

    f[n++] = 0;
    f[n++] = (uint8_t)(sizeof(ssid) - 1);
    k_memcpy(f + n, ssid, sizeof(ssid) - 1);
    n += sizeof(ssid) - 1;
    k_memcpy(f + n, rates, sizeof(rates));
    n += sizeof(rates);
    k_memcpy(f + n, ds, sizeof(ds));
    return n + sizeof(ds);
}

/** The AP's probe response to the station: an open ESS on channel 6. */
static unsigned probe_resp(uint8_t *f)
{
    unsigned n = mgmt(f, 0x50, sta_mac);

    k_memset(f + n, 0, 8); /* timestamp */
    n += 8;
    set16(f + n, 100); /* beacon interval, TUs */
    set16(f + n + 2, WLAN_CAPABILITY_ESS);
    return ap_elems(f, n + 4);
}

/** The AP's beacon, which keeps the association's monitor content. */
static unsigned beacon(uint8_t *f)
{
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    unsigned n = mgmt(f, 0x80, bcast);

    k_memset(f + n, 0, 8);
    n += 8;
    set16(f + n, 100);
    set16(f + n + 2, WLAN_CAPABILITY_ESS);
    return ap_elems(f, n + 4);
}

/* An IPv4 datagram's first bytes, behind an Ethernet header. */
static const uint8_t ip_payload[28] = {0x45, 0,  0,  28, 0, 1, 0, 0, 64, 17, 0, 0, 10, 0,
                                       0,    50, 10, 0,  0, 1, 0, 7, 0,  7,  0, 8, 0,  0};

/** Write an Ethernet frame of type from src to dst with the datagram. */
static unsigned ether(uint8_t *e, const uint8_t *dst, const uint8_t *src, unsigned type)
{
    k_memcpy(e, dst, 6);
    k_memcpy(e + 6, src, 6);
    e[12] = (uint8_t)(type >> 8);
    e[13] = (uint8_t)type;
    k_memcpy(e + 14, ip_payload, sizeof(ip_payload));
    return 14 + sizeof(ip_payload);
}

/* ---- the rows ---- */

static struct wlan *wlan;

/** The radio registers through an open of /dev/wsim and its interface is
 * reachable through nl80211, down until it is brought up. */
static void row_register(void)
{
    struct wsim_info info;

    CHECK(wsim_open(&dev) == -ENODEV);
    CHECK(wlan_attach_wsim() == 0);
    dev_file.status_flags = OFD_NONBLOCK;
    dev.ofd = &dev_file;
    CHECK(wsim_open(&dev) == 0);
    wlan = wlan_at(0);
    CHECK(wlan != 0 && iface == &wlan->netdev && wlan_from_netdev(iface) == wlan);
    if (!wlan) {
        return;
    }
    CHECK(same(iface->mac, sta_mac, 6) && same(iface->name, "wlan0", 6));
    CHECK(family.id != 0 && !wlan->started && !iface->up);

    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    CHECK(rq_send() == -ENETDOWN);
    CHECK(netconf_set_up(iface, 1) == 0);
    CHECK(iface->up && wlan->started);
    CHECK(wsim_ioctl(&dev, WSIM_IOC_INFO, (uintptr_t)&info) == 0 && info.started == 1);
}

/** A scan of channel 6 for any SSID: the probe request goes out, the AP's
 * answer enters the table, and GET_SCAN reports it. */
static void row_scan(void)
{
    const struct nlattr *bss[NL80211_BSS_LAST_SEEN_BOOTTIME + 1];
    uint8_t f[IEEE80211_MAX_FRAME_LEN];
    unsigned len, nest;

    n_events = 0;
    rq_begin(NL80211_CMD_TRIGGER_SCAN, 0);
    nest = req_len;
    rq_put(NL80211_ATTR_SCAN_SSIDS | NLA_F_NESTED, 0, 0);
    rq_put(1, 0, 0);
    set16(req + nest, req_len - nest);
    nest = req_len;
    rq_put(NL80211_ATTR_SCAN_FREQUENCIES | NLA_F_NESTED, 0, 0);
    rq_u32(1, FREQ);
    set16(req + nest, req_len - nest);
    CHECK(rq_send() == 0);
    run_worker();
    CHECK(find_event(GROUP_SCAN, NL80211_CMD_TRIGGER_SCAN) != 0);

    len = take(f);
    CHECK(len >= 24 && f[0] == 0x40); /* a probe request */
    CHECK(same(f + 10, sta_mac, 6) && f[4] == 0xff);
    CHECK(hear(f, probe_resp(f)) > 0);
    run_worker();
    CHECK(wlan->bss_table.count == 1);
    now += WLAN_SCAN_ACTIVE_DWELL_MS + 1;
    run_worker();
    CHECK(find_event(GROUP_SCAN, NL80211_CMD_NEW_SCAN_RESULTS) != 0);
    CHECK(!wlan_scan_running(&wlan->scan));

    rq_begin(NL80211_CMD_GET_SCAN, 1);
    CHECK(rq_send() == 0);
    CHECK(n_replies == 2 && parse(&replies[0]) == NL80211_CMD_NEW_SCAN_RESULTS);
    CHECK(tb[NL80211_ATTR_BSS] != 0);
    if (tb[NL80211_ATTR_BSS]) {
        CHECK(nla_parse_nested(tb[NL80211_ATTR_BSS], 0, NL80211_BSS_LAST_SEEN_BOOTTIME, bss, 0) ==
              0);
        CHECK(has_bytes(bss[NL80211_BSS_BSSID], ap, 6));
        CHECK(bss[NL80211_BSS_FREQUENCY] && nla_get_u32(bss[NL80211_BSS_FREQUENCY]) == FREQ);
    }
}

/** Open System authentication and association with a control port, as
 * wpa_supplicant's SME asks them; only EAPOL passes until SET_STATION
 * authorizes the port. */
static void row_join(void)
{
    static const uint8_t auth_ok[6] = {0, 0, 2, 0, 0, 0};
    uint8_t f[IEEE80211_MAX_FRAME_LEN], e[64];
    uint32_t flags[2];
    unsigned len;
    struct netdev_link link;

    n_events = 0;
    rq_begin(NL80211_CMD_AUTHENTICATE, 0);
    rq_put(NL80211_ATTR_MAC, ap, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, FREQ);
    rq_put(NL80211_ATTR_SSID, ssid, sizeof(ssid) - 1);
    rq_u32(NL80211_ATTR_AUTH_TYPE, NL80211_AUTHTYPE_OPEN_SYSTEM);
    CHECK(rq_send() == 0);
    CHECK(wlan->state == WLAN_STA_AUTHENTICATING && wlan->frequency == FREQ);
    len = take(f);
    CHECK(len == 30 && f[0] == 0xb0 && same(f + 4, ap, 6) && same(f + 10, sta_mac, 6));
    CHECK(f[24] == 0 && f[26] == 1); /* Open System, transaction 1 */
    len = mgmt(f, 0xb0, sta_mac);
    k_memcpy(f + len, auth_ok, 6);
    CHECK(hear(f, len + 6) > 0);
    run_worker();
    CHECK(wlan->state == WLAN_STA_AUTHENTICATED);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_AUTHENTICATE) != 0);
    CHECK(tb[NL80211_ATTR_FRAME] && nla_len(tb[NL80211_ATTR_FRAME]) == 30);

    n_events = 0;
    rq_begin(NL80211_CMD_ASSOCIATE, 0);
    rq_put(NL80211_ATTR_MAC, ap, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, FREQ);
    rq_put(NL80211_ATTR_SSID, ssid, sizeof(ssid) - 1);
    rq_put(NL80211_ATTR_CONTROL_PORT, 0, 0);
    CHECK(rq_send() == 0);
    len = take(f);
    /* The association request, with the SSID first. */
    CHECK(len > 28 && f[0] == 0x00 && same(f + 4, ap, 6));
    CHECK(f[28] == 0 && f[29] == sizeof(ssid) - 1 && same(f + 30, ssid, sizeof(ssid) - 1));
    len = mgmt(f, 0x10, sta_mac);
    set16(f + len, WLAN_CAPABILITY_ESS);
    set16(f + len + 2, WLAN_STATUS_SUCCESS);
    set16(f + len + 4, 0xc001); /* AID 1 */
    len += 6;
    f[len++] = 1;
    f[len++] = 4;
    f[len++] = 0x82;
    f[len++] = 0x84;
    f[len++] = 0x8b;
    f[len++] = 0x96;
    CHECK(hear(f, len) > 0);
    run_worker();
    CHECK(wlan->state == WLAN_STA_ASSOCIATED && wlan->bss && same(wlan->bss->bssid, ap, 6));
    CHECK(find_event(GROUP_MLME, NL80211_CMD_ASSOCIATE) != 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_CONNECT) != 0);
    CHECK(has_bytes(tb[NL80211_ATTR_MAC], ap, 6));
    CHECK(iface->ops->link(iface->dev, &link) == 1 && link.up);

    /* The port is closed: an IP datagram is refused, EAPOL goes out. */
    CHECK(iface->ops->tx(iface->dev, e, (uint16_t)ether(e, ap, sta_mac, 0x0800)) ==
          NETDEV_TX_DROPPED);
    CHECK(iface->ops->tx(iface->dev, e, (uint16_t)ether(e, ap, sta_mac, WLAN_ETH_P_PAE)) ==
          NETDEV_TX_OK);
    run_worker();
    len = take(f);
    CHECK(len == 24 + 8 + sizeof(ip_payload) && (f[0] & 0x0c) == 0x08 && (f[1] & 3) == 1);
    CHECK(f[30] == 0x88 && f[31] == 0x8e);

    rq_begin(NL80211_CMD_SET_STATION, 0);
    rq_put(NL80211_ATTR_MAC, ap, 6);
    flags[0] = flags[1] = 1u << NL80211_STA_FLAG_AUTHORIZED;
    rq_put(NL80211_ATTR_STA_FLAGS2, flags, 8);
    CHECK(rq_send() == 0);
    CHECK(wlan->state == WLAN_STA_AUTHORIZED);
}

/** An IP datagram each way through the authorized port: out as a ToDS
 * frame with LLC/SNAP, in from a FromDS frame to Ethernet. */
static void row_data(void)
{
    uint8_t f[IEEE80211_MAX_FRAME_LEN], e[64];
    unsigned len, n;

    n = ether(e, host, sta_mac, 0x0800);
    CHECK(iface->ops->tx(iface->dev, e, (uint16_t)n) == NETDEV_TX_OK);
    run_worker();
    len = take(f);
    CHECK(len == 24 + 8 + sizeof(ip_payload));
    CHECK((f[0] & 0x0c) == 0x08 && (f[1] & 3) == 1); /* data, ToDS */
    CHECK(same(f + 4, ap, 6) && same(f + 10, sta_mac, 6) && same(f + 16, host, 6));
    CHECK(f[24] == 0xaa && f[25] == 0xaa && f[26] == 3 && f[30] == 0x08 && f[31] == 0);
    CHECK(same(f + 32, ip_payload, sizeof(ip_payload)));

    k_memset(f, 0, 24);
    f[0] = 0x08;
    f[1] = 0x02; /* FromDS */
    k_memcpy(f + 4, sta_mac, 6);
    k_memcpy(f + 10, ap, 6);
    k_memcpy(f + 16, host, 6);
    set16(f + 22, (ap_seq++ & 0xfff) << 4);
    f[24] = f[25] = 0xaa;
    f[26] = 3;
    f[30] = 0x08;
    k_memcpy(f + 32, ip_payload, sizeof(ip_payload));
    n_delivered = 0;
    CHECK(hear(f, 32 + sizeof(ip_payload)) > 0);
    run_worker();
    CHECK(n_delivered == 1 && delivered_len == 14 + sizeof(ip_payload));
    CHECK(same(delivered, sta_mac, 6) && same(delivered + 6, host, 6));
    CHECK(delivered[12] == 0x08 && delivered[13] == 0);
    CHECK(same(delivered + 14, ip_payload, sizeof(ip_payload)));

    /* A beacon keeps the BSS's entry and the association. */
    CHECK(hear(f, beacon(f)) > 0);
    run_worker();
    CHECK(wlan->state == WLAN_STA_AUTHORIZED);
}

/** The interface goes down: the station deauthenticates, nl80211 reports
 * the disconnection, and the radio stops; up again, the radio starts. */
static void row_down(void)
{
    struct netdev_link link;

    n_events = 0;
    CHECK(netconf_set_up(iface, 0) == 0);
    CHECK(!iface->up && !wlan->started && wlan->state == WLAN_STA_NONE && !wlan->bss);
    CHECK(wlan->frequency == 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_DEAUTHENTICATE) != 0);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_DISCONNECT) != 0);
    CHECK(tb[NL80211_ATTR_DISCONNECTED_BY_AP] == 0);
    CHECK(iface->ops->link(iface->dev, &link) == 1 && !link.up);
    CHECK(drain() == 0); /* the stop failed the frame still queued */

    rq_begin(NL80211_CMD_AUTHENTICATE, 0);
    rq_put(NL80211_ATTR_MAC, ap, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, FREQ);
    rq_put(NL80211_ATTR_SSID, ssid, sizeof(ssid) - 1);
    rq_u32(NL80211_ATTR_AUTH_TYPE, NL80211_AUTHTYPE_OPEN_SYSTEM);
    CHECK(rq_send() == -ENETDOWN);
    CHECK(netconf_set_up(iface, 1) == 0 && wlan->started);
}

/** Joined again, an rfkill block leaves the network without a frame and
 * stops the radio; the release starts it again. */
static void row_rfkill(void)
{
    static const uint8_t auth_ok[6] = {0, 0, 2, 0, 0, 0};
    uint8_t f[IEEE80211_MAX_FRAME_LEN];
    unsigned len;
    int index = (int)rfkill_index(wlan->rfkill);

    rq_begin(NL80211_CMD_AUTHENTICATE, 0);
    rq_put(NL80211_ATTR_MAC, ap, 6);
    rq_u32(NL80211_ATTR_WIPHY_FREQ, FREQ);
    rq_put(NL80211_ATTR_SSID, ssid, sizeof(ssid) - 1);
    rq_u32(NL80211_ATTR_AUTH_TYPE, NL80211_AUTHTYPE_OPEN_SYSTEM);
    CHECK(rq_send() == 0);
    CHECK(take(f) == 30);
    len = mgmt(f, 0xb0, sta_mac);
    k_memcpy(f + len, auth_ok, 6);
    CHECK(hear(f, len + 6) > 0);
    run_worker();
    CHECK(wlan->state == WLAN_STA_AUTHENTICATED);

    n_events = 0;
    CHECK(rfkill_store(index, RFKILL_ATTR_SOFT, "1", 1) == 0);
    run_worker();
    CHECK(wlan->blocked && !wlan->started && wlan->state == WLAN_STA_NONE);
    CHECK(find_event(GROUP_MLME, NL80211_CMD_DEAUTHENTICATE) != 0);
    CHECK(drain() == 0);
    CHECK(rfkill_store(index, RFKILL_ATTR_SOFT, "0", 1) == 0);
    run_worker();
    CHECK(!wlan->blocked && wlan->started && iface->up);
}

/** The last close of /dev/wsim unregisters the radio and its interface. */
static void row_close(void)
{
    wsim_close(&dev);
    CHECK(wlan_at(0) == 0 && iface == 0);
}

int main(void)
{
    row_register();
    if (wlan) {
        row_scan();
        row_join();
        row_data();
        row_down();
        row_rfkill();
        row_close();
    }
    if (failures) {
        printf("wlan_join_hosttest: %u failures\n", failures);
        return 1;
    }
    printf("wlan_join_hosttest: ok\n");
    return 0;
}
