/*
 * nd6.c: IPv6 neighbour discovery, duplicate address detection, router
 * discovery and SLAAC, and the interfaces' IPv6 settings, as nd6.h
 * describes them.
 */

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../lib/krandom.h"
#include "../../lib/ksha256.h"
#include "../../mm/kheap.h"
#include "../../klog.h"
#include "../../timer.h"
#include "nd6.h"
#include "mld6.h"
#include "ip6.h"
#include "icmp6.h"
#include "frag6.h"
#include "route6.h"
#include "../net.h"
#include "../netconf.h"
#include "../core/byteorder.h"
#include "../ethernet/ethernet.h"
#include "../driver/netdev.h"
#include "../socket/netlink.h"

/* The low half of the tick count, enough for the intervals here, which
 * are all far shorter than its cycle. */
#define NOW ((uint32_t)kernel_ticks)

/* The protocol's constants (RFC 4861, 10, and RFC 4862, 5.1), in ticks
 * where they are times. */
#define MAX_RTR_SOLICITATIONS     3
#define RTR_SOLICITATION_TICKS    (4 * TICK_HZ)
#define MAX_RTR_SOLICIT_DELAY     TICK_HZ
#define MAX_MULTICAST_SOLICIT     3
#define MAX_UNICAST_SOLICIT       3
#define REACHABLE_TIME_MS         30000u
#define RETRANS_TIMER_MS          1000u
#define DELAY_FIRST_PROBE_TICKS   (5 * TICK_HZ)
#define DUP_ADDR_DETECT_TRANSMITS 1

/* RFC 7217's and RFC 8981's: the identifiers a SLAAC address may try
 * before it gives up on duplicates, the lifetimes of a temporary address,
 * the most its preferred lifetime is shortened by at random, and how long
 * before that lifetime ends its successor is made, in seconds. */
#define IDGEN_RETRIES           3
#define TEMP_VALID_LIFETIME     172800u
#define TEMP_PREFERRED_LIFETIME 86400u
#define MAX_DESYNC_FACTOR       34560u
#define REGEN_ADVANCE           5u

/* The longest a router may set the base reachable time to, an hour. */
#define MAX_REACHABLE_MS 3600000u

/* Two hours, the floor RFC 4862, 5.5.3 (e), puts under a valid lifetime an
 * unauthenticated advertisement shortens. */
#define TWO_HOURS 7200u

/* A neighbour discovery option's types (RFC 4861, 4.6, and RFC 7527). */
#define OPT_SLLA   1
#define OPT_TLLA   2
#define OPT_PREFIX 3
#define OPT_MTU    5
#define OPT_NONCE  14
#define OPT_RDNSS  25
#define OPT_DNSSL  31

/* The flags of an advertisement: from a router, solicited, override. */
#define NA_ROUTER    0x80
#define NA_SOLICITED 0x40
#define NA_OVERRIDE  0x20

/* The flags of a prefix: on the link, for autonomous configuration. */
#define PI_ONLINK 0x80
#define PI_AUTO   0x40

/* The most prefix options, and RDNSS and DNSSL options, one advertisement
 * is read for. */
#define MAX_PREFIXES 8
#define MAX_USEROPTS 8

/* The nonces of the last duplicate detections a card sent, which tell its
 * own solicitation looped back from another node's (RFC 7527). */
#define NONCES 4

/* One cached neighbour; `nd` is NULL in a free entry. */
struct nd6_entry {
    struct netdev *nd;
    uint8_t addr[IP6_ADDR_LEN];
    uint8_t mac[6];
    uint8_t state; /* ND6_* */
    uint8_t router;
    uint8_t probes; /* solicitations sent in this state */
    uint32_t timer; /* the tick of the next step, in INCOMPLETE, REACHABLE, DELAY and PROBE */
    uint32_t used;  /* the tick it was last used or learned, for replacement */
};

/* A datagram that waits for its next hop's station address, a copy on the
 * heap; free when `len` is 0. One past the path MTU is held whole, before
 * fragmentation, so that no fragment of it can put out another. */
struct nd6_hold {
    struct netdev *out;
    uint8_t hop[IP6_ADDR_LEN];
    uint32_t len; /* bytes at `data`, the fixed header included */
    uint32_t mtu; /* the MTU its fragments are cut to, 0 to leave whole */
    uint8_t *data;
};

struct nd6_conf {
    int32_t v[ND6_CONF_KEYS];
    uint8_t secret[16];
    uint8_t secret_set;
};

/* What neighbour discovery keeps for one interface, by its index less 1. */
struct nd6_if {
    struct nd6_conf conf;
    uint8_t started;       /* whether discovery runs, from up until down or disable */
    uint8_t running;       /* the link state the tick saw last */
    uint8_t rs_left;       /* router solicitations still to send */
    uint32_t rs_at;        /* the tick the next may leave */
    uint8_t hlim;          /* the routers' hop limit, 0 for none */
    uint32_t mtu;          /* the routers' MTU, 0 for none */
    uint32_t reachable_ms; /* the routers' base reachable time, 0 for the default */
    uint32_t reachable;    /* ticks, the base time drawn between half and one and a half */
    uint32_t retrans;      /* ticks between solicitations */
    uint8_t nonce[NONCES][6];
    uint8_t next_nonce;
};

static struct nd6_entry cache6[ND6_CACHE_SIZE];
static struct nd6_hold hold6[ND6_HOLD_SLOTS];
static struct nd6_if ifs6[NETDEV_MAX + 1];
static int if_index[NETDEV_MAX + 1];
static struct nd6_conf conf_all, conf_default;

/** Free the datagram held in `h` and the slot. */
static void hold_free(struct nd6_hold *h)
{
    kfree(h->data);
    h->data = 0;
    h->len = 0;
}

/* The key of the random numbers, and the count that makes each draw. */
static uint8_t rand_key[32];
static uint32_t rand_count;

/* The tick the timers run next. */
static uint32_t nd6_next;

/* The messages being sent. */
static uint8_t nd6_msg[64];

static const uint8_t all_routers[IP6_ADDR_LEN] = {0xff, 2, 0, 0, 0, 0, 0, 0,
                                                  0,    0, 0, 0, 0, 0, 0, 2};

/** Whether the `n` bytes at `a` and `b` are the same. */
static int same(const uint8_t *a, const uint8_t *b, int n)
{
    while (n-- > 0) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return 1;
}

/** Fill `n` bytes at `out`, at most 32, with SHA-256 of the key and a new
 * count. */
static void nd6_random(void *out, uint32_t n)
{
    struct ksha256 c;
    uint8_t h[KSHA256_LEN];

    ksha256_init(&c);
    ksha256_update(&c, rand_key, sizeof(rand_key));
    ksha256_update(&c, &rand_count, sizeof(rand_count));
    rand_count++;
    ksha256_final(&c, h);
    k_memcpy(out, h, n > KSHA256_LEN ? KSHA256_LEN : n);
}

uint32_t nd6_random_below(uint32_t n)
{
    uint32_t r;

    nd6_random(&r, sizeof(r));
    return r % n;
}

/** Whether the tick `t` has come. */
static int due(uint32_t t)
{
    return (int32_t)(NOW - t) >= 0;
}

/** The base reachable time of `ni` drawn again between half and one and a
 * half of itself (RFC 4861, 6.3.2). */
static void draw_reachable(struct nd6_if *ni)
{
    uint32_t ms = ni->reachable_ms ? ni->reachable_ms : REACHABLE_TIME_MS;
    uint32_t ticks = ms / 1000u * TICK_HZ + ms % 1000u * TICK_HZ / 1000u;

    ni->reachable = ticks / 2 + nd6_random_below(ticks + 1);
    if (ni->reachable == 0) {
        ni->reachable = 1;
    }
}

/** What a router set on `ni` goes back to the defaults. */
static void if_defaults(struct nd6_if *ni)
{
    ni->hlim = 0;
    ni->mtu = 0;
    ni->reachable_ms = 0;
    ni->retrans = RETRANS_TIMER_MS * TICK_HZ / 1000u;
    draw_reachable(ni);
}

/** The state of interface `nd`, or NULL for NULL or one past the table. */
static struct nd6_if *if_of(const struct netdev *nd)
{
    int slot = netdev_slot(nd) - 1;
    if (!nd || slot < 0 || slot > NETDEV_MAX)
        return 0;
    if (if_index[slot] != nd->index) {
        k_memset(&ifs6[slot], 0, sizeof(ifs6[slot]));
        ifs6[slot].conf = conf_default;
        if_defaults(&ifs6[slot]);
        if_index[slot] = nd->index;
    }
    return &ifs6[slot];
}

static void conf_defaults(struct nd6_conf *c)
{
    k_memset(c, 0, sizeof(*c));
    c->v[ND6_ACCEPT_RA] = 1;
    c->v[ND6_AUTOCONF] = 1;
}

void nd6_init(void)
{
    int i;

    krandom_bytes(rand_key, sizeof(rand_key));
    conf_defaults(&conf_all);
    conf_defaults(&conf_default);
    for (i = 0; i < NETDEV_MAX + 1; i++) {
        ifs6[i].conf = conf_default;
        if_defaults(&ifs6[i]);
    }
}

/* ---- Settings ---- */

/** The settings `which` names, or NULL for an interface that does not
 * exist. */
static struct nd6_conf *conf_of(int which)
{
    if (which == ND6_CONF_ALL) {
        return &conf_all;
    }
    if (which == ND6_CONF_DEFAULT) {
        return &conf_default;
    }
    if (which >= 1 && netdev_by_index(which)) {
        return &if_of(netdev_by_index(which))->conf;
    }
    return 0;
}

int nd6_disabled(const struct netdev *nd)
{
    const struct nd6_if *ni = if_of(nd);

    return ni && ni->conf.v[ND6_DISABLE_IPV6] != 0;
}

int nd6_prefer_temp(const struct netdev *nd)
{
    const struct nd6_if *ni = if_of(nd);

    return ni && ni->conf.v[ND6_USE_TEMPADDR] >= 2;
}

/** Whether `nd` takes router advertisements: accept_ra 1 on a host, or 2. */
static int accepts_ra(const struct nd6_if *ni)
{
    int32_t ra = ni->conf.v[ND6_ACCEPT_RA];

    return ra == 2 || (ra == 1 && !ni->conf.v[ND6_FORWARDING]);
}

int nd6_conf_get(int which, int key, int32_t *value)
{
    struct nd6_conf *c = conf_of(which);

    if (key < 0 || key >= ND6_CONF_KEYS) {
        return -EINVAL;
    }
    if (!c) {
        return -ENODEV;
    }
    *value = c->v[key];
    return 0;
}

static void forget_if(struct netdev *nd, int stop);
static void solicit_routers(struct netdev *nd, struct nd6_if *ni);

/**
 * Put in force on the interface `nd` the change of `key` to `value`: the
 * write itself and what it changes. Called with interrupts on, from a
 * system call.
 */
static void if_set(struct netdev *nd, int key, int32_t value)
{
    struct nd6_if *ni = if_of(nd);
    int32_t old = ni->conf.v[key];
    uint32_t lock;
    int i;

    ni->conf.v[key] = value;
    if (old == value) {
        return;
    }
    if (key == ND6_DISABLE_IPV6 && value) {
        forget_if(nd, 1);
        while (nd->naddr6 > 0) {
            netconf_del_address6(nd, nd->addr6[nd->naddr6 - 1].addr);
        }
    } else if (key == ND6_DISABLE_IPV6) {
        if (nd->loopback) {
            netconf_add_address6(nd, ip6_loopback, 128, NETDEV_A6_PERMANENT,
                                 NETDEV_LIFETIME_FOREVER, NETDEV_LIFETIME_FOREVER);
        } else if (nd->up) {
            netconf_set_up(nd, 1);
        }
    } else if (key == ND6_FORWARDING && !nd->loopback) {
        struct route6_row r;

        lock = net_lock();
        if (value) {
            netdev_join6(nd, all_routers);
            ni->rs_left = 0;
            for (i = 0; route6_at(i, &r);) {
                if (r.ifindex == nd->index && r.proto == ROUTE6_PROTO_RA && r.plen == 0 &&
                    ni->conf.v[ND6_ACCEPT_RA] != 2) {
                    route6_learn(nd, r.dst, 0, r.gw, r.metric, 0);
                } else {
                    i++;
                }
            }
        } else {
            netdev_leave6(nd, all_routers);
            solicit_routers(nd, ni);
        }
        net_unlock(lock);
    }
}

/** `forwarding` and `disable_ipv6` of `all` are written to every interface
 * and to `default`, as Linux writes them. */
int nd6_conf_set(int which, int key, int32_t value)
{
    struct nd6_conf *c = conf_of(which);
    int i;

    if (key < 0 || key >= ND6_CONF_KEYS) {
        return -EINVAL;
    }
    if (!c) {
        return -ENODEV;
    }
    if ((key == ND6_DISABLE_IPV6 || key == ND6_AUTOCONF || key == ND6_FORWARDING) && value != 0 &&
        value != 1) {
        return -EINVAL;
    }
    if (key == ND6_ACCEPT_RA && (value < 0 || value > 2)) {
        return -EINVAL;
    }
    if (which >= 1) {
        if_set(netdev_by_index(which), key, value);
        return 0;
    }
    c->v[key] = value;
    if (which == ND6_CONF_ALL &&
        (key == ND6_FORWARDING || key == ND6_DISABLE_IPV6 || key == ND6_USE_TEMPADDR)) {
        conf_default.v[key] = value;
        for (i = 0; i < netdev_listed() && i < NETDEV_MAX + 1; i++) {
            if_set(netdev_at(i), key, value);
        }
    }
    return 0;
}

int nd6_secret_set(int which, const uint8_t *secret)
{
    struct nd6_conf *c = conf_of(which);
    uint32_t lock;

    if (which == ND6_CONF_ALL) {
        return -EIO;
    }
    if (!c) {
        return -ENODEV;
    }
    lock = net_lock();
    k_memcpy(c->secret, secret, sizeof(c->secret));
    c->secret_set = 1;
    net_unlock(lock);
    return 0;
}

int nd6_secret_get(int which, uint8_t *out)
{
    struct nd6_conf *c = conf_of(which);

    if (which == ND6_CONF_ALL) {
        return -EIO;
    }
    if (!c) {
        return -ENODEV;
    }
    if (!c->secret_set) {
        return -EIO;
    }
    k_memcpy(out, c->secret, sizeof(c->secret));
    return 0;
}

/* ---- The interface's link values ---- */

uint8_t nd6_hop_limit(const struct netdev *nd)
{
    const struct nd6_if *ni = if_of(nd);

    return ni && ni->hlim ? ni->hlim : IP6_HOP_LIMIT_DEFAULT;
}

uint32_t nd6_link_mtu(const struct netdev *nd)
{
    const struct nd6_if *ni = if_of(nd);

    return ni && ni->mtu && ni->mtu < nd->mtu ? ni->mtu : nd->mtu;
}

/* ---- Sending ---- */

/** Put the link-layer address option of `type` for `nd` at `p`; 8 bytes. */
static void put_lladdr(uint8_t *p, uint8_t type, const struct netdev *nd)
{
    p[0] = type;
    p[1] = 1;
    k_memcpy(p + 2, nd->mac, 6);
}

/** A usable link-local address of `nd` into `out`: 1, or 0 for none. */
static int link_local(const struct netdev *nd, uint8_t *out)
{
    int i;

    for (i = 0; i < nd->naddr6; i++) {
        const struct netdev_addr6 *a = &nd->addr6[i];

        if (ip6_is_linklocal(a->addr) &&
            !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED))) {
            k_memcpy(out, a->addr, IP6_ADDR_LEN);
            return 1;
        }
    }
    return 0;
}

/**
 * Solicit `target` on `nd`: to its solicited-node group, or to `target`
 * itself when `unicast`, from `src`, with our link-layer address; or for
 * duplicate detection, a NULL `src`, from :: with a nonce and no address.
 */
static void send_ns(struct netdev *nd, const uint8_t *target, const uint8_t *src, int unicast)
{
    struct nd6_if *ni = if_of(nd);
    uint8_t dst[IP6_ADDR_LEN];

    k_memset(nd6_msg, 0, 32);
    nd6_msg[0] = ND6_NEIGHBOR_SOLICIT;
    k_memcpy(nd6_msg + 8, target, IP6_ADDR_LEN);
    if (unicast) {
        k_memcpy(dst, target, IP6_ADDR_LEN);
    } else {
        ip6_solicited_node(target, dst);
    }
    if (src) {
        put_lladdr(nd6_msg + 24, OPT_SLLA, nd);
        icmp6_send_opts(src, dst, nd->index, nd6_msg, 32, 255, unicast ? IP6_TXF_ONLINK : 0);
        return;
    }
    nd6_msg[24] = OPT_NONCE;
    nd6_msg[25] = 1;
    nd6_random(nd6_msg + 26, 6);
    k_memcpy(ni->nonce[ni->next_nonce], nd6_msg + 26, 6);
    ni->next_nonce = (uint8_t)((ni->next_nonce + 1) % NONCES);
    icmp6_send_opts(0, dst, nd->index, nd6_msg, 32, 255, IP6_TXF_UNSPEC);
}

/** Advertise our `target` on `nd` to `dst`, ff02::1 when NULL, solicited
 * or not, with our link-layer address. */
static void send_na(struct netdev *nd, const uint8_t *target, const uint8_t *dst, int solicited)
{
    struct nd6_if *ni = if_of(nd);

    k_memset(nd6_msg, 0, 32);
    nd6_msg[0] = ND6_NEIGHBOR_ADVERT;
    nd6_msg[4] = NA_OVERRIDE;
    if (solicited) {
        nd6_msg[4] |= NA_SOLICITED;
    }
    if (ni->conf.v[ND6_FORWARDING]) {
        nd6_msg[4] |= NA_ROUTER;
    }
    k_memcpy(nd6_msg + 8, target, IP6_ADDR_LEN);
    put_lladdr(nd6_msg + 24, OPT_TLLA, nd);
    icmp6_send_opts(target, dst ? dst : ip6_all_nodes, nd->index, nd6_msg, 32, 255,
                    dst ? IP6_TXF_ONLINK : 0);
}

/** Solicit routers from `nd`'s link-local address. */
static void send_rs(struct netdev *nd, const uint8_t *src)
{
    k_memset(nd6_msg, 0, 16);
    nd6_msg[0] = ND6_ROUTER_SOLICIT;
    put_lladdr(nd6_msg + 8, OPT_SLLA, nd);
    icmp6_send_opts(src, all_routers, nd->index, nd6_msg, 16, 255, 0);
}

/* ---- The neighbour cache ---- */

static struct nd6_entry *find(const struct netdev *nd, const uint8_t *addr)
{
    int i;

    for (i = 0; i < ND6_CACHE_SIZE; i++) {
        if (cache6[i].nd && (!nd || cache6[i].nd == nd) && ip6_equal(cache6[i].addr, addr)) {
            return &cache6[i];
        }
    }
    return 0;
}

/**
 * A new entry for `addr` on `nd`: a free one, else a failed one, else the
 * one gone longest without use, but never a permanent one; NULL when every
 * entry is permanent. The datagrams held for an entry taken are dropped.
 */
static struct nd6_entry *alloc(struct netdev *nd, const uint8_t *addr)
{
    struct nd6_entry *e = 0;
    int i;

    for (i = 0; i < ND6_CACHE_SIZE; i++) {
        struct nd6_entry *c = &cache6[i];

        if (!c->nd) {
            e = c;
            break;
        }
        if (c->state == ND6_PERMANENT) {
            continue;
        }
        if (!e || (c->state == ND6_FAILED && e->state != ND6_FAILED) ||
            ((c->state == ND6_FAILED) == (e->state == ND6_FAILED) &&
             (int32_t)(c->used - e->used) < 0)) {
            e = c;
        }
    }
    if (!e) {
        return 0;
    }
    if (e->nd) {
        for (i = 0; i < ND6_HOLD_SLOTS; i++) {
            if (hold6[i].len && hold6[i].out == e->nd && ip6_equal(hold6[i].hop, e->addr)) {
                hold_free(&hold6[i]);
            }
        }
    }
    k_memset(e, 0, sizeof(*e));
    e->nd = nd;
    k_memcpy(e->addr, addr, IP6_ADDR_LEN);
    e->used = NOW;
    return e;
}

/** Send the frame of `len` bytes at `dgram` to `mac` by `out`. */
static int xmit(struct netdev *out, const uint8_t *mac, const uint8_t *dgram, uint16_t len)
{
    int sent = ethernet_tx(out, mac, ETH_TYPE_IPV6, dgram, len);

    if (sent != NETDEV_TX_OK) {
        net_mib6.ip6_out_discards++;
        return -ENOBUFS;
    }
    return 0;
}

/** Send the datagrams held for `e`, whose station address is now known. */
static void release(struct nd6_entry *e)
{
    int i;

    for (i = 0; i < ND6_HOLD_SLOTS; i++) {
        struct nd6_hold *h = &hold6[i];

        if (h->len && h->out == e->nd && ip6_equal(h->hop, e->addr)) {
            const uint8_t *d = h->data;

            if (h->mtu) {
                frag6_output(h->out, h->hop, d + 8, d + 24, d[6], d + IP6_HDR_SIZE,
                             (uint16_t)(h->len - IP6_HDR_SIZE), d[7],
                             (uint8_t)(((d[0] & 0x0f) << 4) | (d[1] >> 4)), h->mtu);
            } else {
                xmit(h->out, e->mac, d, (uint16_t)h->len);
            }
            hold_free(h);
        }
    }
}

/** Resolution of `e` failed: answer each datagram held for it with an
 * address unreachable, and leave it FAILED. */
static void fail(struct nd6_entry *e)
{
    int i;

    e->state = ND6_FAILED;
    k_memset(e->mac, 0, 6);
    for (i = 0; i < ND6_HOLD_SLOTS; i++) {
        struct nd6_hold *h = &hold6[i];

        if (h->len && h->out == e->nd && ip6_equal(h->hop, e->addr)) {
            uint16_t len = h->len > 0xffffu ? 0xffffu : (uint16_t)h->len;

            icmp6_unreach_local(h->data, len);
            hold_free(h);
        }
    }
}

/**
 * Keep the datagram of `len` bytes at `data`, a block of the heap the queue
 * now owns, for `hop` on `out`, in a free slot, or in place of the one put
 * in longest ago when none is. `mtu` is the MTU its fragments are cut to
 * when it leaves, or 0 for one that leaves whole.
 */
static void hold(struct netdev *out, const uint8_t *hop, uint8_t *data, uint32_t len, uint32_t mtu)
{
    struct nd6_hold *h = &hold6[0];
    static uint32_t victim;
    int i;

    for (i = 0; i < ND6_HOLD_SLOTS; i++) {
        if (!hold6[i].len) {
            h = &hold6[i];
            break;
        }
    }
    if (i == ND6_HOLD_SLOTS) {
        h = &hold6[victim++ % ND6_HOLD_SLOTS];
        hold_free(h);
    }
    h->out = out;
    k_memcpy(h->hop, hop, IP6_ADDR_LEN);
    h->len = len;
    h->mtu = mtu;
    h->data = data;
}

/** Solicit `e`'s address by multicast, from the held datagram's source
 * `src` when `out` holds it, else from one `ip6_source` chooses. */
static void resolve(struct nd6_entry *e, const uint8_t *src)
{
    uint8_t from[IP6_ADDR_LEN];

    if (src && netdev_holds6(e->nd, src)) {
        k_memcpy(from, src, IP6_ADDR_LEN);
    } else if (ip6_source(e->nd, e->addr, from) != 0) {
        return;
    }
    send_ns(e->nd, e->addr, from, 0);
}

/** Whether `e` has a station address a datagram may leave for at once. */
static int usable(const struct nd6_entry *e)
{
    return e && (e->state & (ND6_REACHABLE | ND6_STALE | ND6_DELAY | ND6_PROBE | ND6_PERMANENT));
}

/**
 * Hold the datagram of `len` bytes at `data`, a block of the heap the queue
 * takes, for `hop` on `out`, whose station address is not known, and ask
 * for it unless a solicitation is out already. `mtu` is as `hold` says.
 * Returns 0, or -ENOBUFS when the cache has no entry to give the hop, the
 * block freed.
 */
static int miss(struct netdev *out, const uint8_t *hop, struct nd6_entry *e, uint8_t *data,
                uint32_t len, uint32_t mtu)
{
    uint8_t src[IP6_ADDR_LEN];

    if (!e) {
        e = alloc(out, hop);
        if (!e) {
            kfree(data);
            net_mib6.ip6_out_discards++;
            return -ENOBUFS;
        }
    }
    k_memcpy(src, data + 8, IP6_ADDR_LEN);
    hold(out, hop, data, len, mtu);
    e->used = NOW;
    if (e->state != ND6_INCOMPLETE) {
        e->state = ND6_INCOMPLETE;
        e->probes = 1;
        e->timer = NOW + if_of(out)->retrans;
        resolve(e, src);
    }
    return 0;
}

int nd6_output(struct netdev *out, const uint8_t *hop, const uint8_t *dgram, uint16_t len)
{
    struct nd6_entry *e = find(out, hop);
    uint8_t *copy;

    if (usable(e)) {
        if (e->state == ND6_STALE) {
            e->state = ND6_DELAY;
            e->timer = NOW + DELAY_FIRST_PROBE_TICKS;
        }
        e->used = NOW;
        return xmit(out, e->mac, dgram, len);
    }
    copy = kmalloc(len);
    if (!copy) {
        net_mib6.ip6_out_discards++;
        return -ENOBUFS;
    }
    k_memcpy(copy, dgram, len);
    return miss(out, hop, e, copy, len, 0);
}

int nd6_output_whole(struct netdev *out, const uint8_t *hop, const uint8_t *hdr,
                     const uint8_t *payload, uint16_t plen, uint32_t mtu, int *held)
{
    struct nd6_entry *e = find(out, hop);
    uint32_t len = (uint32_t)IP6_HDR_SIZE + plen;
    uint8_t *copy;

    *held = 0;
    if (usable(e)) {
        return 0;
    }
    *held = 1;
    copy = kmalloc(len);
    if (!copy) {
        net_mib6.ip6_out_discards++;
        return -ENOBUFS;
    }
    k_memcpy(copy, hdr, IP6_HDR_SIZE);
    k_memcpy(copy + IP6_HDR_SIZE, payload, plen);
    return miss(out, hop, e, copy, len, mtu);
}

/**
 * Learn that `addr` on `nd` is at `mac`, from a solicitation or an
 * advertisement of a router (RFC 4861, 6.3.4 and 7.2.3): a new entry or
 * an INCOMPLETE one takes it and becomes STALE, and its held datagrams
 * leave; one whose address changed becomes STALE; a permanent one is left
 * alone. `router` is 1 for a router's advertisement, else -1.
 */
static void learn(struct netdev *nd, const uint8_t *addr, const uint8_t *mac, int router)
{
    struct nd6_entry *e = find(nd, addr);

    if (!e) {
        e = alloc(nd, addr);
        if (!e) {
            return;
        }
        e->state = ND6_INCOMPLETE;
    }
    if (router >= 0) {
        e->router = (uint8_t)router;
    }
    if (e->state == ND6_PERMANENT) {
        return;
    }
    e->used = NOW;
    if (e->state == ND6_INCOMPLETE || e->state == ND6_FAILED) {
        k_memcpy(e->mac, mac, 6);
        e->state = ND6_STALE;
        release(e);
    } else if (!same(e->mac, mac, 6)) {
        k_memcpy(e->mac, mac, 6);
        e->state = ND6_STALE;
    }
}

void nd6_confirm(struct netdev *nd, const uint8_t *addr)
{
    struct nd6_entry *e = find(nd, addr);

    if (e && (e->state & (ND6_REACHABLE | ND6_STALE | ND6_DELAY | ND6_PROBE))) {
        e->state = ND6_REACHABLE;
        e->timer = NOW + if_of(nd)->reachable;
    }
}

int nd6_router_usable(struct netdev *nd, const uint8_t *addr)
{
    struct nd6_entry *e = find(nd, addr);

    return !e || !(e->state & (ND6_INCOMPLETE | ND6_FAILED));
}

/* ---- Addresses ---- */

/** Whether the identifier at `iid` is one RFC 5453 reserves: the subnet
 * router anycast, the subnet anycasts of RFC 2526, and ISATAP's of RFC
 * 5214. */
static int reserved_iid(const uint8_t *iid)
{
    static const uint8_t anycast[7] = {0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int i, zero = 1;

    for (i = 0; i < 8; i++) {
        zero = zero && iid[i] == 0;
    }
    if (zero) {
        return 1;
    }
    if (same(iid, anycast, 7) && iid[7] >= 0x80) {
        return 1;
    }
    return (iid[0] & ~0x02) == 0 && iid[1] == 0 && iid[2] == 0x5e && iid[3] == 0xfe;
}

/**
 * Write to `addr` the SLAAC address of `nd` in the /64 `prefix` for the
 * DAD_Counter `*count`: RFC 7217's identifier, the first 64 bits of
 * SHA-256 over the prefix, the station address, the counter and the
 * secret, the counter raised past a reserved identifier; or with no secret
 * the modified EUI-64. Returns 1 for RFC 7217's, 0 for the EUI-64.
 */
static int make_address(const struct netdev *nd, const uint8_t *prefix, uint8_t *count,
                        uint8_t *addr)
{
    const struct nd6_if *ni = if_of(nd);
    const uint8_t *secret = ni->conf.secret_set       ? ni->conf.secret
                            : conf_default.secret_set ? conf_default.secret
                                                      : 0;

    k_memcpy(addr, prefix, 8);
    if (!secret) {
        addr[8] = (uint8_t)(nd->mac[0] ^ 0x02);
        addr[9] = nd->mac[1];
        addr[10] = nd->mac[2];
        addr[11] = 0xff;
        addr[12] = 0xfe;
        addr[13] = nd->mac[3];
        addr[14] = nd->mac[4];
        addr[15] = nd->mac[5];
        return 0;
    }
    for (;; (*count)++) {
        struct ksha256 c;
        uint8_t h[KSHA256_LEN];

        ksha256_init(&c);
        ksha256_update(&c, prefix, 8);
        ksha256_update(&c, nd->mac, 6);
        ksha256_update(&c, count, 1);
        ksha256_update(&c, secret, 16);
        ksha256_final(&c, h);
        if (!reserved_iid(h) || *count == 0xff) {
            k_memcpy(addr + 8, h, 8);
            return 1;
        }
    }
}

/** Seconds since the address `a` was given or last renewed, held at
 * 2^32 - 2, short of NETDEV_LIFETIME_FOREVER. */
static uint32_t age(const struct netdev_addr6 *a)
{
    uint64_t t = tick_div(tick_count() - a->since, TICK_HZ, 0);

    return t < 0xFFFFFFFEu ? (uint32_t)t : 0xFFFFFFFEu;
}

/** The seconds left of a lifetime `life` of `a`, FOREVER for none. */
static uint32_t left(const struct netdev_addr6 *a, uint32_t life)
{
    uint32_t t = age(a);

    if (life == NETDEV_LIFETIME_FOREVER) {
        return life;
    }
    return life > t ? life - t : 0;
}

/**
 * Give `nd` the address `addr`/64 made from a router advertisement, with
 * the lifetimes `valid` and `preferred`, its DAD_Counter `count`, and
 * NETDEV_A6_TEMPORARY in `temp` for a temporary one. Returns 0 or the
 * error of `netconf_add_address6`.
 */
static int add_auto(struct netdev *nd, const uint8_t *addr, uint32_t valid, uint32_t preferred,
                    uint8_t count, int temp)
{
    struct netdev_addr6 *a;
    int rc;

    rc = netconf_add_address6(nd, addr, 64, 0, valid, preferred);
    if (rc != 0) {
        return rc;
    }
    a = netdev_addr6_of(nd, addr);
    a->proto = NETDEV_PROTO_KERNEL_RA;
    a->dad_count = count;
    a->flags |= (uint8_t)temp;
    return 0;
}

/**
 * Make a temporary address of RFC 8981, 3.4, in the /64 `prefix` of `nd`,
 * whose lifetimes are `valid` and `preferred`: a random identifier, a valid
 * lifetime of at most two days and a preferred one of at most a day less a
 * random part, so that the hosts of a link do not all renew at once. None
 * is made when that leaves too short a preferred lifetime to be worth it.
 */
static void add_temp(struct netdev *nd, const uint8_t *prefix, uint32_t valid, uint32_t preferred,
                     uint8_t tries)
{
    uint8_t addr[IP6_ADDR_LEN];
    uint32_t tp = TEMP_PREFERRED_LIFETIME - nd6_random_below(MAX_DESYNC_FACTOR);

    if (valid > TEMP_VALID_LIFETIME) {
        valid = TEMP_VALID_LIFETIME;
    }
    if (preferred > tp) {
        preferred = tp;
    }
    if (preferred > valid) {
        preferred = valid;
    }
    if (preferred <= REGEN_ADVANCE) {
        return;
    }
    k_memcpy(addr, prefix, 8);
    do {
        nd6_random(addr + 8, 8);
    } while (reserved_iid(addr + 8));
    add_auto(nd, addr, valid, preferred, tries, NETDEV_A6_TEMPORARY);
}

/** The SLAAC address of `nd`, not temporary, in the /64 of `addr`, or
 * NULL. */
static struct netdev_addr6 *stable_of(struct netdev *nd, const uint8_t *addr)
{
    int i;

    for (i = 0; i < nd->naddr6; i++) {
        struct netdev_addr6 *a = &nd->addr6[i];

        if (a->proto == NETDEV_PROTO_KERNEL_RA && !(a->flags & NETDEV_A6_TEMPORARY) &&
            ip6_prefix_match(a->addr, addr, 64)) {
            return a;
        }
    }
    return 0;
}

/**
 * Renew the address `a` from a prefix with the lifetimes `valid` and
 * `preferred`, by RFC 4862, 5.5.3 (e): a valid lifetime longer than two
 * hours or than what is left is taken; otherwise what is left stands when
 * it is two hours or less, and two hours replaces it when it is more. A
 * temporary address is only ever shortened, so that it keeps its own
 * limits (RFC 8981, 3.4).
 */
static void renew(struct netdev_addr6 *a, uint32_t valid, uint32_t preferred)
{
    uint32_t rv = left(a, a->valid), rp = left(a, a->preferred);

    if (a->flags & NETDEV_A6_TEMPORARY) {
        valid = valid < rv ? valid : rv;
        preferred = preferred < rp ? preferred : rp;
    } else if (!(valid > TWO_HOURS || valid > rv)) {
        valid = rv <= TWO_HOURS ? rv : TWO_HOURS;
    }
    if (preferred > valid) {
        preferred = valid;
    }
    a->valid = valid;
    a->preferred = preferred;
    a->since = tick_count();
    if (preferred == 0) {
        a->flags |= NETDEV_A6_DEPRECATED;
    } else {
        a->flags &= (uint8_t)~NETDEV_A6_DEPRECATED;
    }
}

/**
 * Configure `nd` from the autonomous /64 `prefix` with the lifetimes
 * `valid` and `preferred` (RFC 4862, 5.5.3 (d) and (e)): renew its
 * addresses in the prefix, make the stable one when there is none, and a
 * temporary one when use_tempaddr asks for it and none in the prefix is
 * still preferred.
 */
static void slaac(struct netdev *nd, struct nd6_if *ni, const uint8_t *prefix, uint32_t valid,
                  uint32_t preferred)
{
    uint8_t addr[IP6_ADDR_LEN], count = 0;
    int i, stable = 0, temp = 0;

    for (i = 0; i < nd->naddr6; i++) {
        struct netdev_addr6 *a = &nd->addr6[i];

        if (a->proto != NETDEV_PROTO_KERNEL_RA || !ip6_prefix_match(a->addr, prefix, 64)) {
            continue;
        }
        if (a->flags & NETDEV_A6_TEMPORARY) {
            temp |= !(a->flags & (NETDEV_A6_DEPRECATED | NETDEV_A6_DADFAILED));
        } else {
            stable = 1;
        }
        renew(a, valid, preferred);
    }
    if (valid == 0) {
        return;
    }
    if (!stable) {
        make_address(nd, prefix, &count, addr);
        if (add_auto(nd, addr, valid, preferred, count, 0) == -ENOSPC) {
            klog(KLOG_WARN, "nd6", "%s: no room for a SLAAC address", nd->name);
        }
    }
    if (!temp && ni->conf.v[ND6_USE_TEMPADDR] > 0) {
        add_temp(nd, prefix, valid, preferred, 0);
    }
}

void nd6_dad_start(struct netdev_addr6 *a)
{
    a->dad_left = DUP_ADDR_DETECT_TRANSMITS;
    a->dad_at = NOW + nd6_random_below(MAX_RTR_SOLICIT_DELAY + 1);
}

/**
 * The address `a` of `nd` is a duplicate on the link. A SLAAC address of
 * RFC 7217 is made again with the next DAD_Counter, and a temporary one
 * with a new random identifier, each up to IDGEN_RETRIES times; any other,
 * and one past its retries, is marked NETDEV_A6_DADFAILED and kept, unused,
 * as Linux keeps it.
 */
static void dad_failed(struct netdev *nd, struct netdev_addr6 *a)
{
    uint8_t addr[IP6_ADDR_LEN], prefix[IP6_ADDR_LEN], count = (uint8_t)(a->dad_count + 1);
    uint32_t valid = left(a, a->valid), preferred = left(a, a->preferred);
    int temp = (a->flags & NETDEV_A6_TEMPORARY) != 0;
    char text[40];

    ip6_format(a->addr, text);
    klog(KLOG_WARN, "nd6", "%s: %s is a duplicate on the link", nd->name, text);
    if (a->proto == NETDEV_PROTO_KERNEL_RA && count <= IDGEN_RETRIES && valid > 0) {
        k_memcpy(prefix, a->addr, IP6_ADDR_LEN);
        if (temp) {
            netconf_del_address6(nd, prefix);
            add_temp(nd, prefix, valid, preferred, count);
            return;
        }
        if (make_address(nd, prefix, &count, addr)) {
            netconf_del_address6(nd, prefix);
            add_auto(nd, addr, valid, preferred, count, 0);
            return;
        }
    }
    a->flags |= NETDEV_A6_DADFAILED;
}

/* ---- Receiving ---- */

/* The options of one message, NULL where absent. */
struct nd6_opts {
    const uint8_t *slla, *tlla, *mtu, *nonce;
    const uint8_t *prefix[MAX_PREFIXES];
    int nprefix;
    const uint8_t *useropt[MAX_USEROPTS];
    int nuseropt;
};

/** Read the options in the `len` bytes at `p` into `o`: 1, or 0 for an
 * option of length 0 or past the end, which makes the message invalid
 * (RFC 4861, 4.6). An RDNSS option shorter than one server, or of a length
 * that is not one server's multiple, or a DNSSL option with no domain, is
 * ignored, as RFC 8106, 5, asks. */
static int parse_opts(const uint8_t *p, uint32_t len, struct nd6_opts *o)
{
    k_memset(o, 0, sizeof(*o));
    while (len > 0) {
        uint32_t olen;

        if (len < 2 || p[1] == 0 || (uint32_t)p[1] * 8 > len) {
            return 0;
        }
        olen = (uint32_t)p[1] * 8;
        if (p[0] == OPT_SLLA) {
            o->slla = p + 2;
        } else if (p[0] == OPT_TLLA) {
            o->tlla = p + 2;
        } else if (p[0] == OPT_MTU && olen == 8) {
            o->mtu = p + 4;
        } else if (p[0] == OPT_NONCE && olen == 8) {
            o->nonce = p + 2;
        } else if (p[0] == OPT_PREFIX && olen == 32 && o->nprefix < MAX_PREFIXES) {
            o->prefix[o->nprefix++] = p;
        } else if (((p[0] == OPT_RDNSS && p[1] >= 3 && (p[1] & 1)) ||
                    (p[0] == OPT_DNSSL && p[1] >= 2)) &&
                   o->nuseropt < MAX_USEROPTS) {
            o->useropt[o->nuseropt++] = p;
        }
        p += olen;
        len -= olen;
    }
    return 1;
}

/** Whether `dst` is the solicited-node group of some address. */
static int solicited_node(const uint8_t *dst)
{
    static const uint8_t head[13] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};

    return same(dst, head, 13);
}

/** Whether `nonce` is one `ni` sent. */
static int our_nonce(const struct nd6_if *ni, const uint8_t *nonce)
{
    int i;

    for (i = 0; nonce && i < NONCES; i++) {
        if (same(ni->nonce[i], nonce, 6)) {
            return 1;
        }
    }
    return 0;
}

/**
 * A solicitation (RFC 4861, 7.2.3, and RFC 4862, 5.4.3): one from :: for an
 * address of ours that is tentative is another node's detection of the
 * same address, unless it carries a nonce we sent; one for an address we
 * hold is answered, its sender learned first so that the answer can leave.
 */
static void rx_ns(struct netdev *nd, struct nd6_if *ni, const uint8_t *src, const uint8_t *dst,
                  const uint8_t *msg, uint16_t len)
{
    const uint8_t *target = msg + 8;
    struct netdev_addr6 *a;
    struct nd6_opts o;
    int unspec = ip6_is_unspecified(src);

    net_mib6.icmp6_in_neighbor_solicits++;
    if (len < 24 || ip6_is_multicast(target) || !parse_opts(msg + 24, len - 24u, &o) ||
        (unspec && (!solicited_node(dst) || o.slla))) {
        net_mib6.icmp6_in_errors++;
        return;
    }
    a = netdev_addr6_of(nd, target);
    if (!a) {
        return;
    }
    if (a->flags & NETDEV_A6_TENTATIVE) {
        if (unspec && !(a->flags & NETDEV_A6_DADFAILED) && !our_nonce(ni, o.nonce)) {
            dad_failed(nd, a);
        }
        return;
    }
    if (a->flags & NETDEV_A6_DADFAILED) {
        return;
    }
    if (unspec) {
        send_na(nd, target, 0, 0);
        return;
    }
    if (o.slla) {
        learn(nd, src, o.slla, -1);
    }
    send_na(nd, target, src, 1);
}

/**
 * An advertisement (RFC 4861, 7.2.5, and RFC 4862, 5.4.4): one for an
 * address of ours that is tentative shows it a duplicate; otherwise it
 * updates the neighbour cached for its target, if any, as the solicited
 * and override flags say. A neighbour that stops being a router stops
 * being a default route (7.2.5, last paragraph).
 */
static void rx_na(struct netdev *nd, struct nd6_if *ni, const uint8_t *dst, const uint8_t *msg,
                  uint16_t len)
{
    const uint8_t *target = msg + 8;
    uint8_t flags = msg[4];
    struct netdev_addr6 *a;
    struct nd6_entry *e;
    struct nd6_opts o;
    int kept;

    (void)ni;
    net_mib6.icmp6_in_neighbor_advertisements++;
    if (len < 24 || ip6_is_multicast(target) || !parse_opts(msg + 24, len - 24u, &o) ||
        (ip6_is_multicast(dst) && (flags & NA_SOLICITED))) {
        net_mib6.icmp6_in_errors++;
        return;
    }
    a = netdev_addr6_of(nd, target);
    if (a) {
        if ((a->flags & NETDEV_A6_TENTATIVE) && !(a->flags & NETDEV_A6_DADFAILED)) {
            dad_failed(nd, a);
        }
        return;
    }
    e = find(nd, target);
    if (!e || e->state == ND6_PERMANENT) {
        return;
    }
    if (e->state == ND6_INCOMPLETE || e->state == ND6_FAILED) {
        if (!o.tlla) {
            return;
        }
        k_memcpy(e->mac, o.tlla, 6);
        e->state = (flags & NA_SOLICITED) ? ND6_REACHABLE : ND6_STALE;
        e->timer = NOW + if_of(nd)->reachable;
        e->router = (flags & NA_ROUTER) != 0;
        e->used = NOW;
        release(e);
        return;
    }
    kept = !o.tlla || same(e->mac, o.tlla, 6);
    if (!(flags & NA_OVERRIDE) && !kept) {
        if (e->state == ND6_REACHABLE) {
            e->state = ND6_STALE;
        }
        return;
    }
    if (o.tlla) {
        k_memcpy(e->mac, o.tlla, 6);
    }
    if (flags & NA_SOLICITED) {
        e->state = ND6_REACHABLE;
        e->timer = NOW + if_of(nd)->reachable;
    } else if (!kept) {
        e->state = ND6_STALE;
    }
    if (e->router && !(flags & NA_ROUTER)) {
        route6_learn(nd, ip6_any, 0, target, ROUTE6_METRIC_DEFAULT, 0);
    }
    e->router = (flags & NA_ROUTER) != 0;
}

/**
 * A router advertisement (RFC 4861, 6.3.4): its sender becomes a default
 * route for its router lifetime, its hop limit, timers and MTU are taken,
 * its on-link prefixes become routes, and its autonomous /64 prefixes make
 * addresses (`slaac`). It ends the card's solicitations. Its RDNSS and
 * DNSSL options go to netlink for a program to take the name servers from,
 * on an interface that ignores advertisements too, as Linux sends them.
 */
static void rx_ra(struct netdev *nd, struct nd6_if *ni, const uint8_t *src, const uint8_t *msg,
                  uint16_t len)
{
    struct nd6_opts o;
    uint32_t reach, retrans;
    int i;

    net_mib6.icmp6_in_router_advertisements++;
    if (len < 16 || !ip6_is_linklocal(src) || !parse_opts(msg + 16, len - 16u, &o)) {
        net_mib6.icmp6_in_errors++;
        return;
    }
    for (i = 0; i < o.nuseropt; i++) {
        netlink_nd_useropt(nd, src, o.useropt[i], (uint32_t)o.useropt[i][1] * 8u);
    }
    if (!accepts_ra(ni)) {
        return;
    }
    ni->rs_left = 0;
    if (msg[4] != 0) {
        ni->hlim = msg[4];
    }
    reach = read32be(msg + 8);
    if (reach != 0 && reach <= MAX_REACHABLE_MS && reach != ni->reachable_ms) {
        ni->reachable_ms = reach;
        draw_reachable(ni);
    }
    retrans = read32be(msg + 12);
    if (retrans != 0) {
        ni->retrans = retrans / 1000u * TICK_HZ + retrans % 1000u * TICK_HZ / 1000u;
        if (ni->retrans == 0) {
            ni->retrans = 1;
        }
    }
    if (o.slla) {
        learn(nd, src, o.slla, 1);
    } else if (find(nd, src)) {
        find(nd, src)->router = 1;
    }
    route6_learn(nd, ip6_any, 0, src, ROUTE6_METRIC_DEFAULT, read16be(msg + 6));
    if (o.mtu) {
        uint32_t mtu = read32be(o.mtu);

        if (mtu >= IP6_MTU_MIN && mtu <= nd->mtu) {
            ni->mtu = mtu;
        }
    }
    for (i = 0; i < o.nprefix; i++) {
        const uint8_t *p = o.prefix[i];
        uint32_t valid = read32be(p + 4), preferred = read32be(p + 8);

        if (p[2] > 128 || ip6_is_linklocal(p + 16) || ip6_is_multicast(p + 16) ||
            preferred > valid) {
            continue;
        }
        if (p[3] & PI_ONLINK) {
            route6_learn(nd, p + 16, p[2], 0, ROUTE6_METRIC_PREFIX, valid);
        }
        if ((p[3] & PI_AUTO) && ni->conf.v[ND6_AUTOCONF] && p[2] == 64) {
            slaac(nd, ni, p + 16, valid, preferred);
        }
    }
}

/** The hop limit 255 shows a message was not forwarded, so it came from
 * the link (RFC 4861, 6.1 and 7.1). */
void nd6_rx(struct netdev *nd, const uint8_t *src, const uint8_t *dst, uint8_t hlim,
            const uint8_t *msg, uint16_t len)
{
    struct nd6_if *ni = if_of(nd);

    if (msg[0] == ND6_REDIRECT) {
        net_mib6.icmp6_in_redirects++;
        return;
    }
    if (msg[0] == ND6_ROUTER_SOLICIT) {
        net_mib6.icmp6_in_router_solicits++;
        return;
    }
    if (!nd || nd->loopback || !ni || hlim != 255 || msg[1] != 0) {
        net_mib6.icmp6_in_errors++;
        return;
    }
    if (msg[0] == ND6_NEIGHBOR_SOLICIT) {
        rx_ns(nd, ni, src, dst, msg, len);
    } else if (msg[0] == ND6_NEIGHBOR_ADVERT) {
        rx_na(nd, ni, dst, msg, len);
    } else if (msg[0] == ND6_ROUTER_ADVERT) {
        rx_ra(nd, ni, src, msg, len);
    }
}

/* ---- The interface's life ---- */

/** Start soliciting routers on `nd` when it takes advertisements. */
static void solicit_routers(struct netdev *nd, struct nd6_if *ni)
{
    if (nd->loopback || !accepts_ra(ni)) {
        ni->rs_left = 0;
        return;
    }
    ni->rs_left = MAX_RTR_SOLICITATIONS;
    ni->rs_at = NOW + nd6_random_below(MAX_RTR_SOLICIT_DELAY + 1);
}

/**
 * The tentative addresses, and on a card that went down every address
 * that may be detected, detect again.
 */
static void redetect(struct netdev *nd, int all)
{
    int i;

    for (i = 0; i < nd->naddr6; i++) {
        struct netdev_addr6 *a = &nd->addr6[i];

        if (a->flags & (NETDEV_A6_NODAD | NETDEV_A6_DADFAILED)) {
            continue;
        }
        if (all) {
            a->flags |= NETDEV_A6_TENTATIVE;
        }
        if (a->flags & NETDEV_A6_TENTATIVE) {
            nd6_dad_start(a);
        }
    }
}

void nd6_if_up(struct netdev *nd)
{
    struct nd6_if *ni = if_of(nd);
    uint32_t lock;

    if (!ni || nd->loopback || ni->started) {
        return;
    }
    lock = net_lock();
    ni->started = 1;
    redetect(nd, 1);
    ni->running = (uint8_t)netdev_running(nd);
    if (ni->conf.v[ND6_FORWARDING] && !netdev_member6(nd, all_routers)) {
        netdev_join6(nd, all_routers);
    }
    solicit_routers(nd, ni);
    net_unlock(lock);
}

/**
 * Forget what `nd` learned from its link: its neighbours and the datagrams
 * waiting for them, its routers' routes and settings, and the addresses
 * they made; with `stop`, discovery stops until `nd6_if_up` starts it
 * again. The caller holds no lock, or net_lock.
 */
static void forget_if(struct netdev *nd, int stop)
{
    struct nd6_if *ni = if_of(nd);
    uint32_t lock = net_lock();
    int i;

    for (i = 0; i < ND6_HOLD_SLOTS; i++) {
        if (hold6[i].out == nd) {
            hold_free(&hold6[i]);
        }
    }
    for (i = 0; i < ND6_CACHE_SIZE; i++) {
        if (cache6[i].nd == nd && cache6[i].state != ND6_PERMANENT) {
            cache6[i].nd = 0;
        }
    }
    route6_forget(nd, ROUTE6_PROTO_RA);
    if (ni) {
        ni->started = (uint8_t)(ni->started && !stop);
        ni->rs_left = 0;
        if_defaults(ni);
    }
    for (i = nd->naddr6 - 1; i >= 0; i--) {
        if (i < nd->naddr6 && nd->addr6[i].proto == NETDEV_PROTO_KERNEL_RA) {
            netconf_del_address6(nd, nd->addr6[i].addr);
        }
    }
    net_unlock(lock);
}

void nd6_if_down(struct netdev *nd)
{
    if (nd && !nd->loopback) {
        forget_if(nd, 1);
    }
}

/* ---- Timers ---- */

/** Step the neighbour `e` whose timer has come. */
static void entry_timer(struct nd6_entry *e)
{
    struct nd6_if *ni = if_of(e->nd);
    uint8_t from[IP6_ADDR_LEN];

    switch (e->state) {
    case ND6_INCOMPLETE:
        if (e->probes >= MAX_MULTICAST_SOLICIT) {
            fail(e);
            return;
        }
        e->probes++;
        e->timer = NOW + ni->retrans;
        resolve(e, 0);
        break;
    case ND6_REACHABLE:
        e->state = ND6_STALE;
        break;
    case ND6_DELAY:
        e->state = ND6_PROBE;
        e->probes = 0;
        /* fall through */
    case ND6_PROBE:
        if (e->probes >= MAX_UNICAST_SOLICIT) {
            fail(e);
            return;
        }
        e->probes++;
        e->timer = NOW + ni->retrans;
        if (ip6_source(e->nd, e->addr, from) == 0) {
            send_ns(e->nd, e->addr, from, 1);
        }
        break;
    default:
        break;
    }
}

/**
 * The lifetimes of `nd`'s addresses: an address whose valid lifetime ended
 * is removed, one whose preferred lifetime ended is deprecated, and a
 * temporary address near the end of its preferred lifetime has a successor
 * made, with what is left of its prefix's lifetimes.
 */
static void address_timers(struct netdev *nd, struct nd6_if *ni)
{
    int i;

    for (i = nd->naddr6 - 1; i >= 0; i--) {
        struct netdev_addr6 *a = &nd->addr6[i];
        struct netdev_addr6 *s;
        uint32_t rv = left(a, a->valid), rp = left(a, a->preferred);

        if (rv == 0) {
            char text[40];

            ip6_format(a->addr, text);
            klog(KLOG_INFO, "nd6", "%s: %s expired", nd->name, text);
            netconf_del_address6(nd, a->addr);
            continue;
        }
        if (rp == 0 && !(a->flags & NETDEV_A6_DEPRECATED)) {
            a->flags |= NETDEV_A6_DEPRECATED;
        }
        if ((a->flags & NETDEV_A6_TEMPORARY) && !a->regen && rp <= REGEN_ADVANCE &&
            !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED))) {
            a->regen = 1;
            s = stable_of(nd, a->addr);
            if (s && ni->conf.v[ND6_USE_TEMPADDR] > 0) {
                uint8_t prefix[IP6_ADDR_LEN];

                k_memcpy(prefix, a->addr, IP6_ADDR_LEN);
                add_temp(nd, prefix, left(s, s->valid), left(s, s->preferred), 0);
            }
        }
    }
}

/** Duplicate detection of `nd`'s tentative addresses, while its link is
 * running. */
static void dad_timers(struct netdev *nd)
{
    int i;

    for (i = 0; i < nd->naddr6; i++) {
        struct netdev_addr6 *a = &nd->addr6[i];

        if (!(a->flags & NETDEV_A6_TENTATIVE) || (a->flags & NETDEV_A6_DADFAILED) ||
            !due(a->dad_at)) {
            continue;
        }
        if (a->dad_left > 0) {
            a->dad_left--;
            a->dad_at = NOW + if_of(nd)->retrans;
            send_ns(nd, a->addr, 0, 0);
        } else {
            a->flags &= (uint8_t)~NETDEV_A6_TENTATIVE;
        }
    }
}

/**
 * A card's timers: a link that went down forgets what it learned from its
 * routers, as a card taken down does, since the link that comes back may be
 * another network, and one that came back detects its addresses and
 * solicits routers again, as Linux does on a carrier change; then the
 * tentative addresses, the router solicitations and the lifetimes.
 */
static void if_timers(struct netdev *nd, struct nd6_if *ni)
{
    uint8_t ll[IP6_ADDR_LEN];
    int running = netdev_running(nd);

    if (!running && ni->running) {
        forget_if(nd, 0);
    } else if (running && !ni->running) {
        redetect(nd, 1);
        solicit_routers(nd, ni);
    }
    ni->running = (uint8_t)running;
    if (running) {
        dad_timers(nd);
        if (ni->rs_left > 0 && due(ni->rs_at) && link_local(nd, ll)) {
            ni->rs_left--;
            ni->rs_at = NOW + RTR_SOLICITATION_TICKS;
            send_rs(nd, ll);
        }
    }
    address_timers(nd, ni);
}

void nd6_tick(void)
{
    int i;

    if (!due(nd6_next)) {
        return;
    }
    nd6_next = NOW + (TICK_HZ / 10 ? TICK_HZ / 10 : 1);
    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        struct nd6_if *ni = if_of(nd);

        if (ni && nd->up && !nd6_disabled(nd)) {
            if_timers(nd, ni);
        }
    }
    for (i = 0; i < ND6_CACHE_SIZE; i++) {
        struct nd6_entry *e = &cache6[i];

        if (e->nd && (e->state & (ND6_INCOMPLETE | ND6_REACHABLE | ND6_DELAY | ND6_PROBE)) &&
            due(e->timer)) {
            entry_timer(e);
        }
    }
    route6_tick();
    mld6_tick();
}

/* ---- For the tools ---- */

int nd6_snapshot(struct nd6_row *out, int max)
{
    uint32_t lock;
    int i, n = 0;

    if (!out || max < 1) {
        return 0;
    }
    lock = net_lock();
    for (i = 0; i < ND6_CACHE_SIZE && n < max; i++) {
        struct nd6_entry *e = &cache6[i];

        if (!e->nd) {
            continue;
        }
        out[n].ifindex = e->nd->index;
        k_memcpy(out[n].addr, e->addr, IP6_ADDR_LEN);
        k_memcpy(out[n].mac, e->mac, 6);
        out[n].state = e->state;
        out[n].router = e->router;
        n++;
    }
    net_unlock(lock);
    return n;
}

int nd6_set(struct netdev *nd, const uint8_t *addr, const uint8_t *mac, int permanent)
{
    struct nd6_entry *e;
    uint32_t lock;

    if (!nd || nd->loopback || ip6_is_multicast(addr) || ip6_is_unspecified(addr)) {
        return -EINVAL;
    }
    lock = net_lock();
    e = find(nd, addr);
    if (!e) {
        e = alloc(nd, addr);
    }
    if (!e) {
        net_unlock(lock);
        return -ENOSPC;
    }
    k_memcpy(e->mac, mac, 6);
    e->state = permanent ? ND6_PERMANENT : ND6_STALE;
    e->used = NOW;
    release(e);
    net_unlock(lock);
    return 0;
}

int nd6_forget(struct netdev *nd, const uint8_t *addr)
{
    struct nd6_entry *e;
    uint32_t lock = net_lock();
    int i;

    e = find(nd, addr);
    if (e) {
        for (i = 0; i < ND6_HOLD_SLOTS; i++) {
            if (hold6[i].len && hold6[i].out == e->nd && ip6_equal(hold6[i].hop, e->addr)) {
                hold_free(&hold6[i]);
            }
        }
        e->nd = 0;
    }
    net_unlock(lock);
    return e ? 0 : -ENOENT;
}
