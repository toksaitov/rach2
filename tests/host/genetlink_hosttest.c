/* Exercise generic netlink, its real socket queue and userspace link policy.
 * host-deps: rach2/sys/network/socket/genetlink.c
 * host-deps: rach2/sys/network/socket/address.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 */
#include "../../rach2/sys/network/socket/netlink.c"

extern int printf(const char *format, ...);

static process_t process;
struct percpu cpu_table[NR_CPUS] = {{.current = &process}};
volatile uint64_t kernel_ticks;
static struct netdev card;
static int carrier = 1, privileged = 1, failures, lock_depth, completed, calls;
static int failed_dumps;
static uint8_t request[512], received[32768], storage[8192];

#define CHECK(x)                                                                   \
    do {                                                                           \
        if (!(x)) {                                                                \
            printf("genetlink-test: fail: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

/** Count nested network exclusion; no handler may run while it is held. */
uint32_t net_lock(void)
{
    return (uint32_t)lock_depth++;
}

/** Restore the preceding network exclusion depth. */
void net_unlock(uint32_t flags)
{
    CHECK(lock_depth == (int)flags + 1);
    lock_depth = (int)flags;
}

/** The model has only the initial network namespace. */
int net_ns_seen(void)
{
    return 1;
}
/** Return the privilege chosen by the current row. */
int net_admin(void)
{
    return privileged;
}
/** The model exposes one Ethernet interface. */
int netdev_listed(void)
{
    return 1;
}
/** The model's initial namespace sees its one interface. */
int netdev_seen_count(void)
{
    return 1;
}
/** Return the one interface at index zero. */
struct netdev *netdev_at(int n)
{
    return n == 0 ? &card : 0;
}
/** Report the physical carrier independently of administrative state. */
int netdev_running(const struct netdev *nd)
{
    (void)nd;
    return carrier;
}
/** Validate the model interface's Ethernet MTU. */
int netdev_mtu_ok(const struct netdev *nd, uint32_t mtu)
{
    (void)nd;
    return mtu >= 68 && mtu <= 1500;
}
/** Store an MTU after the caller has checked it. */
int netdev_set_mtu(struct netdev *nd, uint32_t mtu)
{
    nd->mtu = mtu;
    return 0;
}
/** Store administrative state without altering carrier. */
int netconf_set_up(struct netdev *nd, int up)
{
    nd->up = up;
    return 0;
}
/** Count a socket or registry wake by its kind. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    unsigned i;

    for (i = 0; i < WQ_KINDS; ++i) {
        if (kinds & (1u << i)) {
            ++wq->wakes[i];
        }
    }
}
/** Count every kind of wake. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}
/** Require synchronous dispatch to have released the family before removal. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(!lock_depth && cond(arg));
    return 0;
}
/** Return the model tick count. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}
/** Divide model ticks without the kernel's architecture helpers. */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}
/** No IPv6 forwarding option is enabled in this model. */
int nd6_conf_get(int which, int key, int32_t *value)
{
    (void)which;
    (void)key;
    *value = 0;
    return 0;
}
/** Model arp_forget with no addresses, routes or sockets. */
int arp_forget(struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return 0;
}

/** Model arp_set with no addresses, routes or sockets. */
int arp_set(struct netdev *nd, uint32_t ip, const uint8_t *mac, int permanent)
{
    (void)nd;
    (void)ip;
    (void)mac;
    (void)permanent;
    return 0;
}

/** Model arp_snapshot with no addresses, routes or sockets. */
int arp_snapshot(struct arp_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** Model ip6_route with no addresses, routes or sockets. */
struct netdev *ip6_route(const uint8_t *dst, int ifindex, uint8_t *via)
{
    (void)dst;
    (void)ifindex;
    (void)via;
    return 0;
}

/** Model ip6_source with no addresses, routes or sockets. */
int ip6_source(const struct netdev *out, const uint8_t *dst, uint8_t *src)
{
    (void)out;
    (void)dst;
    (void)src;
    return 0;
}

/** Model nd6_forget with no addresses, routes or sockets. */
int nd6_forget(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

/** Model nd6_set with no addresses, routes or sockets. */
int nd6_set(struct netdev *nd, const uint8_t *addr, const uint8_t *mac, int permanent)
{
    (void)nd;
    (void)addr;
    (void)mac;
    (void)permanent;
    return 0;
}

/** Model nd6_snapshot with no addresses, routes or sockets. */
int nd6_snapshot(struct nd6_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** Model netconf_add_address with no addresses, routes or sockets. */
int netconf_add_address(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    (void)nd;
    (void)ip;
    (void)mask;
    return 0;
}

/** Model netconf_add_address6 with no addresses, routes or sockets. */
int netconf_add_address6(struct netdev *nd, const uint8_t *addr, int prefix, int flags,
                         uint32_t valid, uint32_t preferred)
{
    (void)nd;
    (void)addr;
    (void)prefix;
    (void)flags;
    (void)valid;
    (void)preferred;
    return 0;
}

/** Model netconf_del_address with no addresses, routes or sockets. */
int netconf_del_address(struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return 0;
}

/** Model netconf_del_address6 with no addresses, routes or sockets. */
int netconf_del_address6(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

/** Model netconf_set_gateway with no addresses, routes or sockets. */
int netconf_set_gateway(struct netdev *nd, uint32_t gw)
{
    (void)nd;
    (void)gw;
    return 0;
}

/** Model netdev_addr6_of with no addresses, routes or sockets. */
struct netdev_addr6 *netdev_addr6_of(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return 0;
}

/** Model netdev_first with no addresses, routes or sockets. */
struct netdev *netdev_first(void)
{
    return 0;
}

/** Model netdev_holds with no addresses, routes or sockets. */
int netdev_holds(const struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return 0;
}

/** Model netdev_loopback with no addresses, routes or sockets. */
struct netdev *netdev_loopback(void)
{
    return 0;
}

/** Model netdev_on_link with no addresses, routes or sockets. */
int netdev_on_link(const struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return 0;
}

/** Model netdev_owner6 with no addresses, routes or sockets. */
struct netdev *netdev_owner6(const uint8_t *addr)
{
    (void)addr;
    return 0;
}

/** Model netdev_route with no addresses, routes or sockets. */
struct netdev *netdev_route(uint32_t dst, uint32_t *via)
{
    (void)dst;
    (void)via;
    return 0;
}

/** Model netdev_route_add with no addresses, routes or sockets. */
int netdev_route_add(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw, uint32_t metric,
                     int replace)
{
    (void)nd;
    (void)dst;
    (void)mask;
    (void)gw;
    (void)metric;
    (void)replace;
    return 0;
}

/** Model netdev_route_at with no addresses, routes or sockets. */
int netdev_route_at(int n, struct netdev_rt *out)
{
    (void)n;
    (void)out;
    return 0;
}

/** Model netdev_route_del with no addresses, routes or sockets. */
int netdev_route_del(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw)
{
    (void)nd;
    (void)dst;
    (void)mask;
    (void)gw;
    return 0;
}

/** Model netdev_source with no addresses, routes or sockets. */
uint32_t netdev_source(const struct netdev *nd, uint32_t hop)
{
    (void)nd;
    (void)hop;
    return 0;
}

/** Model netif_ns_lo with no addresses, routes or sockets. */
struct netdev *netif_ns_lo(void)
{
    return 0;
}

/** Model netif_ns_lo_set_up with no addresses, routes or sockets. */
void netif_ns_lo_set_up(struct netdev *nd, int up)
{
    (void)nd;
    (void)up;
}

/** Model route6_add with no addresses, routes or sockets. */
int route6_add(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw, uint32_t metric,
               int proto)
{
    (void)nd;
    (void)dst;
    (void)plen;
    (void)gw;
    (void)metric;
    (void)proto;
    return 0;
}

/** Model route6_at with no addresses, routes or sockets. */
int route6_at(int n, struct route6_row *out)
{
    (void)n;
    (void)out;
    return 0;
}

/** Model route6_del with no addresses, routes or sockets. */
int route6_del(struct netdev *nd, const uint8_t *dst, int plen, const uint8_t *gw)
{
    (void)nd;
    (void)dst;
    (void)plen;
    (void)gw;
    return 0;
}

/** Model socket_snapshot with no addresses, routes or sockets. */
int socket_snapshot(int family, int type, struct socket_row **rows)
{
    (void)family;
    (void)type;
    *rows = 0;
    return 0;
}

/** Model tcp_snapshot with no addresses, routes or sockets. */
int tcp_snapshot(struct tcp_row **rows)
{
    *rows = 0;
    return 0;
}

/** Model tcp_state_to_linux with no addresses, routes or sockets. */
int tcp_state_to_linux(int state)
{
    (void)state;
    return 0;
}

/** Model unix_snapshot with no addresses, routes or sockets. */
int unix_snapshot(struct unix_row **rows)
{
    *rows = 0;
    return 0;
}

static const struct nla_policy policy[3] = {
    {GENL_NLA_UNSPEC, 0, 0}, {GENL_NLA_U32, 0, 0}, {GENL_NLA_STRING, 1, 16}};
static struct genl_multicast_group groups[40];

/** Echo a validated scalar through the real unicast path. */
static int echo(const struct genl_info *info)
{
    struct genl_msg msg;
    uint8_t buf[64];

    CHECK(!lock_depth && info->cmd == 1 && info->userhdr[0] == 0x5a);
    CHECK(info->attrs[1] && nla_get_u32(info->attrs[1]) == 0x12345678u);
    ++calls;
    genl_msg_init(&msg, buf, sizeof(buf));
    genl_msg_begin(&msg, info->family, 1, info->seq, 0, 0);
    nla_put_u32(&msg, 1, nla_get_u32(info->attrs[1]));
    genl_msg_end(&msg);
    return genl_reply(info, &msg);
}

/** Return two pages with a stable cursor, the second completing the dump. */
static int enumerate(struct genl_dump *dump, struct genl_msg *msg)
{
    CHECK(!lock_depth && dump->info.userhdr[0] == 0x5a);
    genl_msg_begin(msg, dump->info.family, 2, dump->info.seq, 0, 0);
    nla_put_u32(msg, 1, (uint32_t)dump->args[0]);
    genl_msg_end(msg);
    ++dump->args[0];
    return dump->args[0] < 2;
}

/** Count exactly one cleanup for each started dump. */
static void finish_dump(struct genl_dump *dump)
{
    CHECK(!lock_depth && dump->args[0] == 2);
    ++completed;
}

/** Reject a semantic error with a borrowed explanation and byte offset. */
static int reject(const struct genl_info *info)
{
    *info->extack = "rejected test value";
    *info->bad_offset = 24;
    return -EINVAL;
}

/** Fail a started dump after the dispatcher has acquired its storage. */
static int failed_dump(struct genl_dump *dump, struct genl_msg *msg)
{
    (void)msg;
    CHECK(!lock_depth && dump->args[0] == 0);
    return -EIO;
}

/** Reject a callback that claims more output without advancing the dump. */
static int empty_dump(struct genl_dump *dump, struct genl_msg *msg)
{
    (void)dump;
    (void)msg;
    return 1;
}

/** Count cleanup of a failed or empty callback. */
static void failed_done(struct genl_dump *dump)
{
    CHECK(!lock_depth && dump->args[0] == 0);
    ++failed_dumps;
}

static const struct genl_ops operations[] = {{1, GENL_ADMIN_PERM, 0, echo, 0, 0},
                                             {2, 0, 0, 0, enumerate, finish_dump},
                                             {3, 0, 0, reject, 0, 0},
                                             {4, 0, 0, 0, failed_dump, failed_done},
                                             {5, 0, 0, 0, empty_dump, failed_done}};
static struct genl_family family = {"test_family", 0, 7,      1,  2, policy,
                                    operations,    5, groups, 40, 0, 0};

/** Build a request independently of generic message encoding. */
static void make_request(unsigned type, unsigned cmd, unsigned flags, unsigned len)
{
    k_memset(request, 0, sizeof(request));
    put32(request, len);
    put16(request + 4, (uint16_t)type);
    put16(request + 6, (uint16_t)(flags | 1u));
    put32(request + 8, 0x1234);
    put32(request + 12, 0xdeadbeef);
    request[16] = (uint8_t)cmd;
    request[17] = 7;
    request[20] = 0x5a;
}

/** Read one whole queued datagram, leaving no fragment behind. */
static int take(struct socket *sock)
{
    int n = netlink_pending(sock);

    CHECK(n > 0 && (unsigned)n <= sizeof(received));
    if (n <= 0 || (unsigned)n > sizeof(received)) {
        return 0;
    }
    CHECK(netlink_peek(sock, 0, received, sizeof(received)) == n);
    netlink_consume(sock);
    return n;
}

/** Empty a model socket's queue. */
static void drain(struct socket *sock)
{
    while (netlink_pending(sock) >= 0) {
        (void)take(sock);
    }
}

/** Exercise family discovery, callbacks, framing and extended errors. */
static void requests(struct socket *sock)
{
    const struct nlattr *attrs[8], *ops[6], *item[3], *mcast[41];
    struct genl_msg msg;
    unsigned offset, n;

    make_request(GENL_ID_CTRL, 3, 0, 36);
    k_memset(request + 20, 0, 16);
    put16(request + 20, 16);
    put16(request + 22, 2);
    k_memcpy(request + 24, "test_family", 12);
    CHECK(netlink_send(sock, request, 36) == 36);
    CHECK(take(sock) > 20 && get16(received + 4) == GENL_ID_CTRL);
    CHECK(get32(received + 8) == 0x1234 && received[16] == 1 && received[17] == 2);
    n = get32(received);
    CHECK(nla_parse(received + 20, n - 20, 0, 7, attrs, 0) == 0);
    CHECK(nla_get_u16(attrs[1]) == family.id && nla_get_u32(attrs[3]) == 7);
    CHECK(nla_get_u32(attrs[4]) == 1 && nla_get_u32(attrs[5]) == 2);
    CHECK(nla_parse_nested(attrs[6], 0, 5, ops, 0) == 0);
    CHECK(nla_parse_nested(ops[1], 0, 2, item, 0) == 0);
    CHECK(nla_get_u32(item[1]) == 1 && nla_get_u32(item[2]) == 11);
    CHECK(nla_parse_nested(attrs[7], 0, 40, mcast, 0) == 0);
    CHECK(nla_parse_nested(mcast[40], 0, 2, item, 0) == 0);
    CHECK(nla_get_u32(item[2]) == groups[39].id);

    make_request(GENL_ID_CTRL, 3, 0x300, 20);
    CHECK(netlink_send(sock, request, 20) == 20);
    CHECK(take(sock) > 20 && (get16(received + 6) & 2));
    CHECK(take(sock) > 20 && (get16(received + 6) & 2));
    CHECK(take(sock) == 20 && get16(received + 4) == 3);

    make_request(family.id, 1, 4, 32);
    put16(request + 24, 8);
    put16(request + 26, 1);
    put32(request + 28, 0x12345678);
    CHECK(netlink_send(sock, request, 32) == 32 && calls == 1);
    CHECK(take(sock) == 32 && get16(received + 4) == family.id &&
          get32(received + 12) == state_of(sock)->pid);
    CHECK(take(sock) == 36 && get16(received + 4) == 2 && get32(received + 16) == 0);
    CHECK(get16(received + 6) & 0x100);
    privileged = 0;
    CHECK(netlink_send(sock, request, 32) == 32 && calls == 1);
    CHECK(take(sock) == 52 && (int32_t)get32(received + 16) == -EPERM);
    privileged = 1;

    make_request(family.id, 2, 0x304, 24);
    CHECK(netlink_send(sock, request, 24) == 24 && completed == 1);
    CHECK(take(sock) == 32 && (get16(received + 6) & 2) && get32(received + 28) == 0);
    CHECK(take(sock) == 32 && (get16(received + 6) & 2) && get32(received + 28) == 1);
    CHECK(take(sock) == 20 && get16(received + 4) == 3 && get32(received + 16) == 0);
    CHECK(netlink_pending(sock) == -1);
    make_request(family.id, 4, 0x300, 24);
    CHECK(netlink_send(sock, request, 24) == 24 && failed_dumps == 1);
    CHECK(take(sock) == 20 && get16(received + 4) == 3 && (int32_t)get32(received + 16) == -EIO);
    make_request(family.id, 5, 0x300, 24);
    CHECK(netlink_send(sock, request, 24) == 24 && failed_dumps == 2);
    CHECK(take(sock) == 20 && (int32_t)get32(received + 16) == -EMSGSIZE);

    CHECK(netlink_setsockopt(sock, 11, 1, 4) == 0);
    make_request(family.id, 3, 0, 24);
    CHECK(netlink_send(sock, request, 24) == 24);
    n = (unsigned)take(sock);
    CHECK(n > 44 && get16(received + 6) == 0x200);
    CHECK(nla_parse(received + 44, n - 44, 0, 2, item, 0) == 0);
    CHECK(nla_get_u32(item[2]) == 24);
    CHECK(netlink_setsockopt(sock, 10, 1, 4) == 0);
    CHECK(netlink_send(sock, request, 24) == 24);
    n = (unsigned)take(sock);
    CHECK(n > 36 && get16(received + 6) == 0x300);
    CHECK(nla_parse(received + 36, n - 36, 0, 2, item, 0) == 0);
    CHECK(nla_get_u32(item[2]) == 24);

    make_request(family.id, 1, 0, 32);
    put16(request + 24, 6);
    put16(request + 26, 1);
    CHECK(netlink_send(sock, request, 32) == 32 && calls == 1);
    n = (unsigned)take(sock);
    CHECK((int32_t)get32(received + 16) == -EINVAL);
    CHECK(nla_parse(received + 36, n - 36, 0, 2, item, 0) == 0);
    CHECK(nla_get_u32(item[2]) == 24);

    genl_msg_init(&msg, storage + 1, 64);
    CHECK(nla_put_u64(&msg, 1, 0x0123456789abcdefULL) == 0);
    CHECK(nla_parse(storage + 1, msg.len, 0, 2, item, &offset) == 0);
    CHECK(nla_get_u64(item[1]) == 0x0123456789abcdefULL);
    CHECK(nla_put_u32(&msg, 1, 7) == 0);
    CHECK(nla_parse(storage + 1, msg.len, 0, 2, item, &offset) == -EINVAL);
    CHECK(!item[0] && !item[1] && !item[2] && offset == 12);
    genl_msg_init(&msg, storage, 4);
    CHECK(nla_put_u32(&msg, 1, 1) == -EMSGSIZE && msg.len == 0);
    CHECK(nla_put_flag(&msg, 2) == -EMSGSIZE && msg.len == 0);
}

/** Exercise all recipients, memberships above 32 and a slow reader's overrun. */
static void multicast(struct socket *a, struct socket *b)
{
    struct genl_msg msg;
    unsigned i;

    CHECK(netlink_bind(a, 0, 1) == 0);
    CHECK(netlink_setsockopt(b, 1, 1, 4) == 0);
    {
        uint8_t name[12];

        netlink_name(b, name);
        CHECK(get32(name + 8) == 1);
    }
    CHECK(netlink_setsockopt(a, 1, (int)groups[39].id, 4) == 0);
    CHECK(netlink_setsockopt(b, 1, (int)groups[39].id, 4) == 0);
    CHECK(netlink_setsockopt(b, 1, 2000, 4) == -EINVAL);
    CHECK(netlink_setsockopt(b, 1, 1, 3) == -EINVAL);
    genl_msg_init(&msg, storage, sizeof(storage));
    genl_msg_begin(&msg, &family, 1, 0, 0, 0);
    genl_msg_end(&msg);
    CHECK(genl_multicast(&family, 0, &msg) == 0);
    CHECK(take(a) == 24 && take(b) == 24);
    CHECK(genl_multicast(&family, 39, &msg) == 0);
    CHECK(take(a) == 24 && take(b) == 24);
    CHECK(netlink_setsockopt(a, 2, (int)groups[39].id, 4) == 0);
    CHECK(genl_multicast(&family, 39, &msg) == 0);
    CHECK(netlink_pending(a) == -1 && take(b) == 24);
    CHECK(netlink_setsockopt(a, 1, (int)groups[39].id, 4) == 0);
    genl_msg_init(&msg, storage, sizeof(storage));
    genl_msg_begin(&msg, &family, 1, 0, 0, 0);
    nla_put(&msg, 1, received, 4000);
    genl_msg_end(&msg);
    for (i = 0; i < 16; ++i) {
        CHECK(genl_multicast(&family, 39, &msg) == 0);
        CHECK(take(a) == (int)msg.len);
    }
    CHECK(genl_multicast(&family, 39, &msg) == -ENOBUFS);
    CHECK(!a->so_error && b->so_error == ENOBUFS);
    CHECK(take(a) == (int)msg.len);
    CHECK(state_of(b)->queued == 16 * msg.len);
    drain(b);
    b->so_error = 0;
    CHECK(netlink_setsockopt(b, 2, (int)groups[39].id, 4) == 0);
    CHECK(netlink_setsockopt(a, 2, (int)groups[39].id, 4) == 0);
    CHECK(genl_multicast(&family, 39, &msg) == 0);
    CHECK(netlink_pending(a) == -1 && netlink_pending(b) == -1);
}

/** Read the two link policy attributes from a real RTM_NEWLINK response. */
static void check_link(struct socket *sock, unsigned mode, unsigned oper, int running)
{
    const struct nlattr *attrs[24];
    unsigned n = (unsigned)take(sock);

    CHECK(get16(received + 4) == 16 && n > 32);
    CHECK(nla_parse(received + 32, n - 32, 0, 23, attrs, 0) == 0);
    CHECK(nla_get_u8(attrs[16]) == oper && nla_get_u8(attrs[17]) == mode);
    CHECK(((get32(received + 24) & IFF_RUNNING) != 0) == running);
}

/** Exercise the supplicant's dormant/up transitions and route notifications. */
static void link_policy_test(struct socket *route, struct socket *watch)
{
    CHECK(netlink_bind(watch, 0, 1) == 0);
    make_request(19, 0, 4, 48);
    k_memset(request + 16, 0, 32);
    put32(request + 20, 1);
    put16(request + 32, 5);
    put16(request + 34, 17);
    request[36] = 1;
    put16(request + 40, 5);
    put16(request + 42, 16);
    request[44] = 5;
    CHECK(netlink_send(route, request, 48) == 48);
    CHECK(take(route) == 36 && get32(received + 16) == 0);
    check_link(watch, 1, 5, 0);
    request[44] = 6;
    CHECK(netlink_send(route, request, 48) == 48);
    CHECK(take(route) == 36 && get32(received + 16) == 0);
    check_link(watch, 1, 6, 1);
    carrier = 0;
    netlink_tick();
    check_link(watch, 1, 2, 0);
    carrier = 1;
    kernel_ticks += 1000;
    netlink_tick();
    check_link(watch, 1, 5, 0);
    request[36] = 0;
    request[44] = 9;
    CHECK(netlink_send(route, request, 48) == 48);
    CHECK(take(route) == 36 && (int32_t)get32(received + 16) == -EINVAL);
    CHECK(netlink_pending(watch) == -1);
    request[44] = 6;
    CHECK(netlink_send(route, request, 48) == 48);
    (void)take(route);
    check_link(watch, 0, 6, 1);
}

/** Register a family and run every row with all sockets and storage released. */
int main(void)
{
    struct socket a, b, route, watch;
    struct genl_family duplicate;
    char names[40][8];
    unsigned i, old_id;

    process.tgid = 77;
    card.index = 1;
    card.up = 1;
    card.mtu = 1500;
    k_memcpy(card.name, "eth0", 5);
    k_memset(&a, 0, sizeof(a));
    k_memset(&b, 0, sizeof(b));
    k_memset(&route, 0, sizeof(route));
    k_memset(&watch, 0, sizeof(watch));
    for (i = 0; i < 40; ++i) {
        names[i][0] = 'g';
        names[i][1] = (char)('0' + i / 10);
        names[i][2] = (char)('0' + i % 10);
        names[i][3] = 0;
        groups[i].name = names[i];
    }
    CHECK(genl_register_family(&family) == 0 && family.id >= 0x13);
    duplicate = family;
    duplicate.id = 0;
    CHECK(genl_register_family(&duplicate) == -EEXIST && duplicate.id == 0);
    CHECK(netlink_open(&a, NETLINK_GENERIC) == 0 && netlink_open(&b, NETLINK_GENERIC) == 0);
    CHECK(netlink_open(&route, NETLINK_ROUTE) == 0 && netlink_open(&watch, NETLINK_ROUTE) == 0);
    CHECK(netlink_bind(&a, 77, 0) == 0 && netlink_bind(&route, 77, 0) == 0);
    CHECK(netlink_bind(&b, 77, 0) == -EADDRINUSE);
    requests(&a);
    multicast(&a, &b);
    link_policy_test(&route, &watch);
    old_id = family.id;
    genl_unregister_family(&family);
    CHECK(!family.id && !groups[39].id);
    make_request(old_id, 1, 0, 24);
    CHECK(netlink_send(&a, request, 24) == 24);
    CHECK(take(&a) > 36 && (int32_t)get32(received + 16) == -ENOENT);
    CHECK(netlink_setsockopt(&a, 1, 40, 4) == -EINVAL);
    genl_unregister_family(&family);
    netlink_close(&a);
    netlink_close(&b);
    netlink_close(&route);
    netlink_close(&watch);
    CHECK(!states && !lock_depth && completed == 1);
    printf("genetlink-test: result: %s\n", failures ? "failed" : "passed");
    return failures != 0;
}
