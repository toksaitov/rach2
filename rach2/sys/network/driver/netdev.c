/*
 * netdev.c: the driver table, the probe that brings up the cards and `lo`,
 * and the interface records it fills, as netdev.h describes them.
 *
 * The order of the table is the probe's order, and so the order of the
 * interfaces' names on a machine with cards of more than one kind: the
 * RTL8139, which rach2/qemu_devices.sh gives QEMU unless `NIC` names
 * another card; the e1000, which QEMU attaches when no card is named; the
 * RTL8168GU, the Lenovo G50-70's card, which QEMU does not emulate; and the
 * virtio network device, which the suites give most of their machines. The
 * cards a PC has come first, so a machine that has one beside a virtio
 * device, as one that gives the virtio device to a guest, keeps the card as
 * its first interface.
 */

#include "../../idle_timer.h"
#include "netdev.h"
#include "e1000.h"
#include "rtl8139.h"
#include "rtl8168.h"
#include "virtio_net.h"
#include "../net.h"
#include "../netconf.h"
#include "../arp/arp.h"
#include "../ip6/ip6.h"
#include "../ip6/nd6.h"
#include "../ip6/route6.h"
#include "../socket/packet.h"
#include "../../cmdline.h"
#include "../../fs/synthfs.h"
#include "../../fs/sysfs.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"
#include "../../pci.h"
#include "../../timer.h"
#include "../../udelay.h"
#include "../../vfio/vfio.h"

/* The interfaces: the `niface` cards the probe brought up, in the order it
 * found them, and `lo` after them; then the `nextra` interfaces registered
 * since (`netdev_register`). A card of the probe's that was unregistered
 * since, as a virtio network device unbound, has its bit in `gone`: it is
 * listed no more, and its record is never used again, so that a pointer to
 * it kept somewhere reaches no other interface. */
static struct netdev ifaces[NETDEV_MAX + 1];
static int niface;
static unsigned gone;
static struct netdev *extra[NETDEV_MAX];
static int nextra;
static int next_index;

/* Whether `lo` stands after the cards, in ifaces[niface]. */
static int lo_ready;

/* What the probe keeps of each card, so that its driver can let the card go
 * and take it back (`netdev_release`, `netdev_reclaim`): the driver, the
 * card's number among the driver's, whether the card is away, and the table
 * that stands in for the driver's while it is. */
static const struct netdev_ops *card_ops[NETDEV_MAX];
static int card_nth[NETDEV_MAX];
static int card_away[NETDEV_MAX];
static struct netdev_ops away_ops[NETDEV_MAX];

/* The reason the last probe's last failed card gave (`netdev_error`). A
 * driver that answers absent leaves it as it is, so that one card's absence
 * does not hide another's failure, the one report a machine with no network
 * has. */
static const char *probe_error = "no supported network card";

static const struct netdev_ops *const drivers[] = {&rtl8139_netdev, &e1000_netdev, &rtl8168_netdev,
                                                   &virtio_net_netdev};

#define NETDEV_COUNT ((int)(sizeof(drivers) / sizeof(drivers[0])))

/** The probe's cards still listed. */
static int probe_listed(void)
{
    int i, n = 0;

    for (i = 0; i < niface; i++) {
        n += !(gone & 1u << i);
    }
    return n;
}

int netdev_count(void)
{
    return probe_listed() + nextra;
}

struct netdev *netdev_at(int n)
{
    int i, cards = probe_listed();

    if (n < 0) {
        return 0;
    }
    if (n < cards) {
        for (i = 0; i < niface; i++) {
            if (!(gone & 1u << i) && n-- == 0) {
                return &ifaces[i];
            }
        }
    }
    if (n >= cards && n < cards + nextra)
        return extra[n - cards];
    return n == cards + nextra && lo_ready ? &ifaces[niface] : 0;
}

int netdev_listed(void)
{
    return netdev_count() + lo_ready;
}

struct netdev *netdev_loopback(void)
{
    return lo_ready ? &ifaces[niface] : 0;
}

/** `lo` sends nothing itself: a frame a packet socket gives it is dropped. */
static int loopback_tx(void *dev, const uint8_t *data, uint16_t len)
{
    (void)dev;
    (void)data;
    (void)len;
    return NETDEV_TX_DROPPED;
}

static const struct netdev_ops loopback_ops = {"loopback", 0, loopback_tx, 0, 0, 0, 0, 0, 0, 0};

void netdev_loopback_init(struct netdev *nd, int index)
{
    k_memset(nd, 0, sizeof(*nd));
    nd->ops = &loopback_ops;
    nd->index = index;
    nd->name[0] = 'l';
    nd->name[1] = 'o';
    nd->pci_key = -1;
    nd->loopback = 1;
    nd->mtu = NETDEV_LOOPBACK_MTU;
}

/** Put `lo` after the cards the probe brought up. */
static void loopback_up(void)
{
    struct netdev *nd = &ifaces[niface];

    netdev_loopback_init(nd, niface + 1);
    nd->ip = 0x7F000001u;
    nd->mask = 0xFF000000u;
    nd->addr6[0].addr[15] = 1; /* ::1 */
    nd->addr6[0].prefix = 128;
    nd->addr6[0].scope = NETDEV_SCOPE_HOST;
    nd->addr6[0].flags = NETDEV_A6_PERMANENT;
    nd->addr6[0].proto = NETDEV_PROTO_KERNEL_LO;
    nd->addr6[0].valid = NETDEV_LIFETIME_FOREVER;
    nd->addr6[0].preferred = NETDEV_LIFETIME_FOREVER;
    nd->naddr6 = 1;
    nd->up = 1;
    lo_ready = 1;
    next_index = niface + 2;
}

/** Copy the IPv6 address at `from` to `to`. */
static void copy6(uint8_t *to, const uint8_t *from)
{
    int i;

    for (i = 0; i < 16; i++) {
        to[i] = from[i];
    }
}

/** Whether the IPv6 addresses at `a` and `b` are the same. */
static int same6(const uint8_t *a, const uint8_t *b)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

struct netdev_addr6 *netdev_addr6_of(struct netdev *nd, const uint8_t *addr)
{
    int i;

    for (i = 0; nd && i < nd->naddr6; i++) {
        if (same6(nd->addr6[i].addr, addr)) {
            return &nd->addr6[i];
        }
    }
    return 0;
}

int netdev_holds6(struct netdev *nd, const uint8_t *addr)
{
    struct netdev_addr6 *a = netdev_addr6_of(nd, addr);

    return a && !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED));
}

struct netdev *netdev_owner6(const uint8_t *addr)
{
    int i;

    for (i = 0; i < netdev_listed(); i++) {
        struct netdev *nd = netdev_at(i);
        struct netdev_addr6 *a = netdev_addr6_of(nd, addr);
        if (nd->up && a && !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)) &&
            !(a->deleted_routes & NETDEV_A6_LOCAL_ROUTE))
            return nd;
    }
    return route6_added_local(addr);
}

/** The slot of `group` among `nd`'s groups, or -1. */
static int group_slot(const struct netdev *nd, const uint8_t *group)
{
    int i;

    for (i = 0; i < nd->ngroup6; i++) {
        if (same6(nd->group6[i], group)) {
            return i;
        }
    }
    return -1;
}

int netdev_join6(struct netdev *nd, const uint8_t *group)
{
    int i;

    if (!nd || group[0] != 0xff) {
        return -EINVAL;
    }
    i = group_slot(nd, group);
    if (i >= 0) {
        if (nd->group6_refs[i] == 0xff) {
            return -ENOSPC;
        }
        nd->group6_refs[i]++;
        return 0;
    }
    if (nd->ngroup6 == NETDEV_GROUPS6) {
        return -ENOSPC;
    }
    copy6(nd->group6[nd->ngroup6], group);
    nd->group6_refs[nd->ngroup6] = 1;
    nd->ngroup6++;
    return 0;
}

/** The last slot moves into the one freed, since the order of the groups
 * means nothing. */
void netdev_leave6(struct netdev *nd, const uint8_t *group)
{
    int i = nd ? group_slot(nd, group) : -1;

    if (i < 0 || --nd->group6_refs[i] != 0) {
        return;
    }
    nd->ngroup6--;
    if (i != nd->ngroup6) {
        copy6(nd->group6[i], nd->group6[nd->ngroup6]);
        nd->group6_refs[i] = nd->group6_refs[nd->ngroup6];
    }
}

int netdev_member6(const struct netdev *nd, const uint8_t *group)
{
    return nd && group_slot(nd, group) >= 0;
}

struct netdev *netdev_first(void)
{
    return netdev_count() > 0 ? netdev_at(0) : 0;
}

struct netdev *netdev_by_name(const char *name)
{
    int i;

    for (i = 0; i < netdev_listed(); i++) {
        const char *a = netdev_at(i)->name;
        const char *b = name;

        while (*a != 0 && *a == *b) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) {
            return netdev_at(i);
        }
    }
    return 0;
}

int netdev_holds(const struct netdev *nd, uint32_t ip)
{
    int i;

    if (ip == 0 || !nd) {
        return 0;
    }
    for (i = 0; i < nd->naddr2; i++) {
        if (nd->ip2[i] == ip) {
            return 1;
        }
    }
    return nd->ip == ip;
}

int netdev_on_link(const struct netdev *nd, uint32_t ip)
{
    int i;

    for (i = -1; i < nd->naddr2; i++) {
        uint32_t local = i < 0 ? nd->ip : nd->ip2[i];
        uint32_t mask = i < 0 ? nd->mask : nd->mask2[i];
        uint32_t dst = netdev_addr_dst(nd, i);

        if (nd->up && !nd->addr4[i + 1].noprefix && !(nd->addr4[i + 1].deleted & ADDR4_PREFIX) &&
            !netdev_secondary(nd, i) && (ip & mask) == (dst & mask) &&
            netdev_addr_has_route(local, dst, mask)) {
            return 1;
        }
    }
    return 0;
}

uint32_t netdev_source(const struct netdev *nd, uint32_t hop)
{
    uint32_t source = nd->ip, best_mask = 0;
    int i;

    for (i = -1; i < nd->naddr2; i++) {
        uint32_t local = i < 0 ? nd->ip : nd->ip2[i];
        uint32_t mask = i < 0 ? nd->mask : nd->mask2[i];

        if (local != 0 && mask > best_mask && (hop & mask) == (netdev_addr_dst(nd, i) & mask)) {
            if (!nd->l3) {
                return local;
            }
            source = local;
            best_mask = mask;
        }
    }
    return source;
}

int netdev_owns(uint32_t ip)
{
    int i, a;
    if (!ip)
        return 0;
    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        for (a = -1; a < nd->naddr2; a++)
            if (netdev_addr_local(nd, a, ADDR4_LOCAL) == ip)
                return 1;
    }
    return netdev_added_local(ip, NETDEV_RT_LOCAL) != 0;
}

int netdev_is_broadcast(uint32_t ip)
{
    int i, a;

    if (ip == 0xFFFFFFFFu) {
        return 1;
    }
    for (i = 0; i < netdev_count(); i++) {
        const struct netdev *nd = netdev_at(i);

        for (a = -1; a < nd->naddr2; a++) {
            if (ip && (ip == netdev_addr_local(nd, a, ADDR4_BROADCAST) ||
                       ip == netdev_addr_local(nd, a, ADDR4_SUBNET_BROADCAST)))
                return 1;
        }
    }
    return ip && netdev_added_local(ip, NETDEV_RT_BROADCAST) != 0;
}

/** Name `nd`, the interface of card `n` from 0, `ethN`, for N up to 99. */
static void name_iface(struct netdev *nd, int n)
{
    nd->name[0] = 'e';
    nd->name[1] = 't';
    nd->name[2] = 'h';
    if (n < 10) {
        nd->name[3] = (char)('0' + n);
        nd->name[4] = 0;
    } else {
        nd->name[3] = (char)('0' + n / 10);
        nd->name[4] = (char)('0' + n % 10);
        nd->name[5] = 0;
    }
}

/**
 * The number from 0 of the card `rach2.nicfail=N` refuses, or -1 when the
 * boot line has no such option or its value is not a decimal number. Only
 * the cards that come up are counted.
 */
static int nicfail_at(void)
{
    char value[8];
    int n = 0;
    int i;

    if (!kcmdline_value("rach2.nicfail", value, (unsigned int)sizeof(value))) {
        return -1;
    }
    for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
        n = n * 10 + (value[i] - '0');
    }
    return i > 0 && value[i] == '\0' ? n : -1;
}

/**
 * A card that fails is stopped here, since its bring-up may have enabled
 * bus mastering and no interface is left through which the shutdown could
 * stop it; its slot of `ifaces` is given to the next card. Its reason, the
 * driver's error or the boot line's refusal, replaces `probe_error`. The
 * limit of `NETDEV_MAX` failures ends a driver that never answers absent.
 */
int netdev_probe(void)
{
    int i;
    int fail_at = nicfail_at();
    int found = 0;

    niface = 0;
    probe_error = "no supported network card";

    for (i = 0; i < NETDEV_COUNT; i++) {
        int nth;
        int failures = 0;

        for (nth = 0; niface < NETDEV_MAX && failures < NETDEV_MAX; nth++) {
            struct netdev *nd = &ifaces[niface];
            int verdict;
            int refused;

            k_memset(nd, 0, sizeof(*nd));
            nd->ops = drivers[i];
            nd->index = niface + 1;
            nd->pci_key = -1;
            nd->mtu = NETDEV_ETH_MTU_MAX;
            name_iface(nd, niface);
            verdict = drivers[i]->init(nd, nth);

            refused = verdict == NETDEV_UP && found++ == fail_at;
            if (refused) {
                klog(KLOG_WARN, "net", "%s: refused: rach2.nicfail=%d", nd->name, fail_at);
                verdict = NETDEV_FAILED;
                probe_error = "the card was refused on the boot line";
            }
            if (verdict == NETDEV_ABSENT) {
                break;
            }
            if (verdict == NETDEV_FAILED) {
                if (drivers[i]->stop) {
                    drivers[i]->stop(nd->dev);
                }
                if (!refused && drivers[i]->error) {
                    probe_error = drivers[i]->error();
                }
                failures++;
                continue;
            }
            nd->up = 1;
            card_ops[niface] = drivers[i];
            card_nth[niface] = nth;
            card_away[niface] = 0;
            if (niface == 0) {
                int b;

                for (b = 0; b < 6; b++) {
                    net_mac[b] = nd->mac[b];
                }
            }
            niface++;
        }
    }
    loopback_up();
    return niface > 0;
}

int netdev_mtu_ok(const struct netdev *nd, uint32_t mtu)
{
    return nd->loopback ? mtu == NETDEV_LOOPBACK_MTU
                        : mtu >= NETDEV_ETH_MTU_MIN &&
                              mtu <= (nd->mtu_max ? nd->mtu_max : NETDEV_ETH_MTU_MAX);
}

int netdev_set_mtu(struct netdev *nd, uint32_t mtu)
{
    if (!netdev_mtu_ok(nd, mtu)) {
        return -EINVAL;
    }
    if (mtu != nd->mtu) {
        klog(KLOG_INFO, "net", "%s: mtu: %d", nd->name, (int)mtu);
    }
    nd->mtu = mtu;
    return 0;
}

int netdev_tx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    if (nd && nd->ops->tx) {
        return nd->ops->tx(nd->dev, data, len);
    }
    return NETDEV_TX_DROPPED;
}

void netdev_poll(void)
{
    struct netdev *nd;
    int i;

    for (i = 0; (nd = netdev_at(i)) != 0 && !nd->loopback; i++) {
        if (nd->ops->poll) {
            nd->ops->poll(nd->dev);
        }
    }
}

const char *netdev_name(struct netdev *nd)
{
    return nd ? nd->ops->name : "none";
}

const char *netdev_error(void)
{
    return probe_error;
}

void netdev_print_info(struct netdev *nd)
{
    if (nd && nd->ops->print_info) {
        nd->ops->print_info(nd->dev);
    }
}

void netdev_stop(struct netdev *nd)
{
    if (nd && nd->ops->stop) {
        nd->ops->stop(nd->dev);
    }
}

int netdev_link(const struct netdev *nd, struct netdev_link *out)
{
    if (nd && nd->ops->link) {
        return nd->ops->link(nd->dev, out);
    }
    return 0;
}

/* The ticks a reading of a card's link that found it up stands for, as
 * Linux's e1000 reads its link from its watchdog every two seconds and at a
 * link change's interrupt, and `netif_carrier_ok` reads only the state
 * kept (drivers/net/ethernet/intel/e1000/e1000_main.c:2547 and 3757,
 * include/linux/netdevice.h:4257, Linux 6.12.115). */
#define NETDEV_CARRIER_TICKS (2u * TICK_HZ)

/**
 * Whether `nd`'s carrier is up, as netdev.h describes it. A reading that
 * found the link up stands for NETDEV_CARRIER_TICKS, and a reading that
 * found it down for none, so the card's register is read at most once a
 * window while the link stays up: under KVM it is a register access that
 * QEMU serves in its own process, which waits for QEMU's main lock and
 * took some 300 us in each pass of IPv6's timers during a graphics round.
 * Only the probe's cards keep a reading, each written through the table,
 * whose records are not const; an interface registered beside them, a
 * radio's, reports a state its driver keeps in memory and is asked each
 * time. The words need no
 * lock of their own: a lost write costs one more reading, and a reading
 * that raced a link change keeps the old state one window at most, as
 * Linux's watchdog does between its runs.
 */
int netdev_running(const struct netdev *nd)
{
    struct netdev_link link;
    struct netdev *card = 0;
    uint32_t now = (uint32_t)kernel_ticks;
    int i;

    if (!nd) {
        return 0;
    }
    if (nd->carrier_kept && (int32_t)(nd->carrier_until - now) > 0) {
        return 1;
    }
    if (netdev_link(nd, &link)) {
        for (i = 0; i < niface; i++) {
            if (&ifaces[i] == nd) {
                card = &ifaces[i];
            }
        }
        if (card) {
            card->carrier_until = now + NETDEV_CARRIER_TICKS;
            card->carrier_kept = link.up ? 1 : 0;
        }
        return link.up ? 1 : 0;
    }
    return 1;
}

void netdev_carrier_changed(struct netdev *nd)
{
    if (nd) {
        nd->carrier_kept = 0;
    }
}

void netdev_name_eth(struct netdev *nd)
{
    int n;

    for (n = 0; n < 99; n++) {
        name_iface(nd, n);
        if (!netdev_by_name(nd->name)) {
            return;
        }
    }
}

/** Attach an interface after the probe without moving any existing interface
 * record. */
int netdev_register(struct netdev *nd)
{
    unsigned used = 0;
    int i, slot;

    if (!nd || !nd->ops || !nd->ops->tx || !nd->name[0] || netdev_by_name(nd->name))
        return -EINVAL;
    if (netdev_count() >= NETDEV_MAX || next_index <= 0 || next_index == 0x7fffffff)
        return -ENOSPC;
    for (i = 0; i < netdev_listed(); ++i)
        used |= 1u << netdev_slot(netdev_at(i));
    for (slot = 1; slot <= NETDEV_MAX + 1 && (used & (1u << slot)); ++slot) {
    }
    nd->slot = slot;
    nd->index = next_index++;
    extra[nextra++] = nd;
    return 0;
}

/** Withdraw an interface after its driver and work have stopped: a card of
 * the probe's is marked gone, an attached interface taken off the list. */
void netdev_unregister(struct netdev *nd)
{
    int i;

    for (i = 0; i < niface; i++) {
        if (&ifaces[i] == nd) {
            gone |= 1u << i;
            return;
        }
    }
    for (i = 0; i < nextra && extra[i] != nd; ++i) {
    }
    if (i == nextra)
        return;
    for (; i + 1 < nextra; ++i)
        extra[i] = extra[i + 1];
    extra[--nextra] = 0;
}

/* The neighbours `netdev_withdraw` reads, under net_lock. */
static struct arp_row withdraw_arp[ARP_CACHE_SIZE];
static struct nd6_row withdraw_nd6[ND6_CACHE_SIZE];

/** Flush the device's IPv4 routes and remove all its addresses before its
 * driver leaves. A card without an address may still have explicit routes. */
static void drop_addresses(struct netdev *nd)
{
    nd6_if_down(nd);
    netdev_route_flush(nd, 1);
    while (nd->ip)
        (void)netconf_set_address(nd, 0, 0);
}

void netdev_withdraw(struct netdev *nd)
{
    uint32_t flags;
    int n, count;

    flags = net_lock();
    (void)netconf_set_up(nd, 0);
    drop_addresses(nd);
    arp_forget_dev(nd);
    count = arp_snapshot(withdraw_arp, ARP_CACHE_SIZE);
    for (n = 0; n < count; ++n) {
        if (withdraw_arp[n].iface == nd->name) {
            (void)arp_forget(nd, withdraw_arp[n].ip);
        }
    }
    count = nd6_snapshot(withdraw_nd6, ND6_CACHE_SIZE);
    for (n = 0; n < count; ++n) {
        if (withdraw_nd6[n].ifindex == nd->index) {
            (void)nd6_forget(nd, withdraw_nd6[n].addr);
        }
    }
    ip6_forget_dev(nd);
    packet_dev_gone(nd->index);
    netdev_unregister(nd);
    net_unlock(flags);
}

/** Return the next service deadline for the stopped idle tick. */
uint32_t netdev_idle_ticks(void)
{
    struct netdev *nd;
    int i;

    for (i = 0; (nd = netdev_at(i)) != 0 && !nd->loopback; i++) {
        if (nd->ops->poll) {
            return 1;
        }
    }
    return TICK_HZ;
}

/** The `tx` of a card that is away: every frame is dropped. */
static int away_tx(void *dev, const uint8_t *data, uint16_t len)
{
    (void)dev;
    (void)data;
    (void)len;
    return NETDEV_TX_DROPPED;
}

/** The `link` of a card that is away: no carrier. */
static int away_link(void *dev, struct netdev_link *out)
{
    (void)dev;
    out->up = 0;
    out->mbps = 0;
    out->full_duplex = 0;
    return 1;
}

/** The `set_up` of a card that is away: it may go down, not up, since
 * nothing would carry what it sends. */
static int away_set_up(void *dev, int up)
{
    (void)dev;
    return up ? -ENODEV : 0;
}

/** The number of the probe's card `nd` in `ifaces`, or -1 for an interface
 * that is no card of the probe's or one unregistered since. */
static int card_of(const struct netdev *nd)
{
    int i;

    for (i = 0; i < niface; i++) {
        if (&ifaces[i] == nd && !(gone & 1u << i)) {
            return i;
        }
    }
    return -1;
}

int netdev_release(struct netdev *nd)
{
    int i = card_of(nd);
    uint32_t flags;
    uint8_t bus, devfn;

    if (i < 0 || card_away[i] || nd->pci_key < 0 || !card_ops[i]->stop) {
        return -ENODEV;
    }
    bus = (uint8_t)(nd->pci_key >> 8);
    devfn = (uint8_t)nd->pci_key;
    (void)netconf_set_up(nd, 0);
    flags = net_lock();
    drop_addresses(nd);
    away_ops[i].name = card_ops[i]->name;
    away_ops[i].tx = away_tx;
    away_ops[i].link = away_link;
    away_ops[i].set_up = away_set_up;
    nd->ops = &away_ops[i];
    card_away[i] = 1;
    netdev_carrier_changed(nd);
    net_unlock(flags);
    card_ops[i]->stop(nd->dev);
    pci_unclaim(bus, devfn, PCI_OWNER_KERNEL);
    sysfs_pci_unbind(nd->pci_key);
    (void)sysfs_pci_uevent(nd->pci_key, "unbind");
    klog(KLOG_INFO, "net", "%s: released: the card is free for another driver", nd->name);
    return 0;
}

/**
 * Bring the function at `bus`, `devfn` to D0 before its driver touches it.
 *
 * A guest's driver may leave the card in D3hot, as Linux's r8169 does when
 * its system powers off. Nothing else brings it back: VFIO's saved state has
 * no power word, and a card with no function-level reset and No_Soft_Reset
 * set is reset by nobody. In D3hot the card's window reads all ones, which
 * the bring-up takes for no card. The wait is the one a change from D3hot
 * needs.
 */
static void card_wake(uint8_t bus, uint8_t devfn)
{
    pci_device_t dev;

    k_memset(&dev, 0, sizeof(dev));
    dev.bus = bus;
    dev.device = (uint8_t)(devfn >> 3);
    dev.function = (uint8_t)(devfn & 7u);
    if (pci_power_state(&dev) > PCI_D0) {
        (void)pci_set_power_state(&dev, PCI_D0);
        k_udelay(PCI_PM_D3HOT_WAIT * 1000u);
        klog(KLOG_INFO, "net", "pci %x:%x.%u: brought back to D0", (unsigned)bus,
             (unsigned)dev.device, (unsigned)dev.function);
    }
}

int netdev_reclaim(struct netdev *nd, const char *driver)
{
    int i = card_of(nd);
    uint32_t flags;
    uint8_t bus, devfn;
    int key;
    int tries;

    if (i < 0 || !card_away[i]) {
        return -ENODEV;
    }
    key = nd->pci_key;
    bus = (uint8_t)(key >> 8);
    devfn = (uint8_t)key;
    if (pci_owner(bus, devfn) != NULL) {
        return -EBUSY;
    }
    card_wake(bus, devfn);
    /* A card a guest died on may need the first bring-up's reset to start. */
    for (tries = 0;; tries++) {
        if (card_ops[i]->init(nd, card_nth[i]) == NETDEV_UP && nd->pci_key == key) {
            break;
        }
        klog(KLOG_WARN, "net", "%s: not taken back: %s", nd->name,
             card_ops[i]->error ? card_ops[i]->error() : "the card did not start");
        card_ops[i]->stop(nd->dev);
        nd->pci_key = key;
        pci_unclaim(bus, devfn, PCI_OWNER_KERNEL);
        if (tries == 1) {
            return -EIO;
        }
        k_udelay(100000u);
    }
    if (driver) {
        int r = sysfs_pci_bind(key, driver);

        if (r != 0) {
            klog(KLOG_WARN, "net", "%s: not taken back: /sys would not show it bound: error %d",
                 nd->name, r);
            card_ops[i]->stop(nd->dev);
            nd->pci_key = key;
            pci_unclaim(bus, devfn, PCI_OWNER_KERNEL);
            return r;
        }
        (void)sysfs_pci_uevent(key, "bind");
    }
    flags = net_lock();
    nd->ops = card_ops[i];
    card_away[i] = 0;
    netdev_carrier_changed(nd);
    net_unlock(flags);
    klog(KLOG_INFO, "net", "%s: taken back by its driver", nd->name);
    return 0;
}

/* The drivers that let a card go, under the names Linux's drivers of the
 * same cards have in /sys/bus/pci/drivers. */
static const struct {
    const struct netdev_ops *ops;
    const char *name;
} sys_drivers[] = {{&e1000_netdev, "e1000"}, {&rtl8168_netdev, "r8169"}};

#define SYS_DRIVERS ((int)(sizeof(sys_drivers) / sizeof(sys_drivers[0])))

/** The value of the hexadecimal digit `c`, or -1. */
static int hex_digit(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/**
 * The card of driver `n` of `sys_drivers` that the text at `buf` names,
 * `0000:BB:DD.F` with perhaps a newline after it, as a write of `bind` or
 * `unbind` gives it; NULL for other text, or a function that is no card of
 * that driver.
 */
static struct netdev *card_named(int n, const char *buf, uint32_t len)
{
    int b1, b0, d1, d0, f, i, key;

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
        len--;
    }
    if (len != 12 || buf[0] != '0' || buf[1] != '0' || buf[2] != '0' || buf[3] != '0' ||
        buf[4] != ':' || buf[7] != ':' || buf[10] != '.') {
        return 0;
    }
    b1 = hex_digit(buf[5]);
    b0 = hex_digit(buf[6]);
    d1 = hex_digit(buf[8]);
    d0 = hex_digit(buf[9]);
    f = hex_digit(buf[11]);
    if (b1 < 0 || b0 < 0 || d1 < 0 || d0 < 0 || f < 0 || d1 > 1 || f > 7) {
        return 0;
    }
    key = (b1 << 4 | b0) << 8 | (d1 << 4 | d0) << 3 | f;
    for (i = 0; i < niface; i++) {
        if (ifaces[i].pci_key == key && card_ops[i] == sys_drivers[n].ops && !(gone & 1u << i)) {
            return &ifaces[i];
        }
    }
    return 0;
}

/** /sys/bus/pci/drivers/DRIVER/unbind: let go of the card named
 * (`netdev_release`); -ENODEV for a function the driver does not have. */
static int32_t store_unbind(void *obj, int arg, const char *buf, uint32_t len)
{
    struct netdev *nd = card_named(arg, buf, len);
    int r;

    (void)obj;
    if (!nd) {
        return -ENODEV;
    }
    r = netdev_release(nd);
    return r != 0 ? r : (int32_t)len;
}

/** /sys/bus/pci/drivers/DRIVER/bind: take back the card named
 * (`netdev_reclaim`); -ENODEV for a function that was never the driver's,
 * whose driver_override names another driver, or that a driver has,
 * vfio-pci among them, as Linux's bind_store answers for a device that
 * does not match or has a driver (drivers/base/bus.c). */
static int32_t store_bind(void *obj, int arg, const char *buf, uint32_t len)
{
    struct netdev *nd = card_named(arg, buf, len);
    int r;

    (void)obj;
    if (!nd || !vfio_override_allows(nd->pci_key, sys_drivers[arg].name)) {
        return -ENODEV;
    }
    r = netdev_reclaim(nd, sys_drivers[arg].name);
    if (r == -EBUSY) {
        r = -ENODEV;
    }
    return r != 0 ? r : (int32_t)len;
}

static const struct sysfs_attr attr_bind = {"bind", 0200u, NULL, store_bind};
static const struct sysfs_attr attr_unbind = {"unbind", 0200u, NULL, store_unbind};

/** The directory `name` below `parent`, made when it is not there yet: its
 * handle, or -errno. */
static int dir_at(int parent, const char *name)
{
    int h;

    if (parent <= 0) {
        return parent < 0 ? parent : -ENOENT;
    }
    h = sysfs_node_child(parent, name, (int)k_strlen(name));
    return h > 0 ? h : sysfs_add_dir(parent, name);
}

void netdev_sys_start(void)
{
    int n, i;

    for (n = 0; n < SYS_DRIVERS; n++) {
        int dir = dir_at(dir_at(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers"), sys_drivers[n].name);

        if (dir < 0 || sysfs_add_attr(dir, &attr_bind, NULL, n) < 0 ||
            sysfs_add_attr(dir, &attr_unbind, NULL, n) < 0) {
            klog(KLOG_WARN, "net", "/sys/bus/pci/drivers/%s not made: error %d",
                 sys_drivers[n].name, dir < 0 ? dir : -ENOMEM);
            continue;
        }
        for (i = 0; i < niface; i++) {
            if (card_ops[i] == sys_drivers[n].ops && ifaces[i].pci_key >= 0) {
                (void)sysfs_pci_bind(ifaces[i].pci_key, sys_drivers[n].name);
            }
        }
    }
}
