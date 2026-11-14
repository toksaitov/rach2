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
#include "../../cmdline.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"
#include "../../timer.h"

/* The interfaces: the `niface` cards the probe brought up, in the order it
 * found them, and `lo` after them. */
static struct netdev ifaces[NETDEV_MAX + 1];
static int niface;
static struct netdev *extra[NETDEV_MAX];
static int nextra;
static int next_index;

/* Whether `lo` stands after the cards, in ifaces[niface]. */
static int lo_ready;

/* The reason the last probe's last failed card gave (`netdev_error`). A
 * driver that answers absent leaves it as it is, so that one card's absence
 * does not hide another's failure, the one report a machine with no network
 * has. */
static const char *probe_error = "no supported network card";

static const struct netdev_ops *const drivers[] = {&rtl8139_netdev, &e1000_netdev, &rtl8168_netdev,
                                                   &virtio_net_netdev};

#define NETDEV_COUNT ((int)(sizeof(drivers) / sizeof(drivers[0])))

int netdev_count(void)
{
    return niface + nextra;
}

struct netdev *netdev_at(int n)
{
    if (n >= 0 && n < niface)
        return &ifaces[n];
    if (n >= niface && n < niface + nextra)
        return extra[n - niface];
    return n == niface + nextra && lo_ready ? &ifaces[niface] : 0;
}

int netdev_listed(void)
{
    return niface + nextra + lo_ready;
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
        if (netdev_holds6(netdev_at(i), addr)) {
            return netdev_at(i);
        }
    }
    return 0;
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
    return niface > 0 ? &ifaces[0] : nextra ? extra[0] : 0;
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

    for (i = 0; i < nd->naddr2; i++) {
        if (nd->mask2[i] != 0xFFFFFFFFu && (ip & nd->mask2[i]) == (nd->ip2[i] & nd->mask2[i])) {
            return 1;
        }
    }
    return nd->ip != 0 && nd->mask != 0 && nd->mask != 0xFFFFFFFFu &&
           (ip & nd->mask) == (nd->ip & nd->mask);
}

uint32_t netdev_source(const struct netdev *nd, uint32_t hop)
{
    int i;

    if (nd->ip != 0 && nd->mask != 0 && (hop & nd->mask) == (nd->ip & nd->mask)) {
        return nd->ip;
    }
    for (i = 0; i < nd->naddr2; i++) {
        if ((hop & nd->mask2[i]) == (nd->ip2[i] & nd->mask2[i])) {
            return nd->ip2[i];
        }
    }
    return nd->ip;
}

int netdev_owns(uint32_t ip)
{
    int i;

    for (i = 0; i < netdev_count(); i++) {
        if (netdev_holds(netdev_at(i), ip)) {
            return 1;
        }
    }
    return 0;
}

int netdev_is_broadcast(uint32_t ip)
{
    int i, a;

    if (ip == 0xFFFFFFFFu) {
        return 1;
    }
    for (i = 0; i < netdev_count(); i++) {
        const struct netdev *nd = netdev_at(i);

        if (nd->ip != 0 && NETDEV_MASK_HAS_BROADCAST(nd->mask) && ip == (nd->ip | ~nd->mask)) {
            return 1;
        }
        for (a = 0; a < nd->naddr2; a++) {
            if (NETDEV_MASK_HAS_BROADCAST(nd->mask2[a]) && ip == (nd->ip2[a] | ~nd->mask2[a])) {
                return 1;
            }
        }
    }
    return 0;
}

/** Name `nd`, the interface of card `n` from 0, `ethN`. */
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
    int i;

    for (i = 0; i < niface; i++) {
        if (ifaces[i].ops->poll) {
            ifaces[i].ops->poll(ifaces[i].dev);
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

/** Attach a non-PCI interface without moving any existing interface record. */
int netdev_register(struct netdev *nd)
{
    unsigned used = 0;
    int i, slot;

    if (!nd || !nd->ops || !nd->ops->tx || !nd->name[0] || netdev_by_name(nd->name))
        return -EINVAL;
    if (niface + nextra >= NETDEV_MAX || next_index <= 0 || next_index == 0x7fffffff)
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

/** Withdraw an attached interface after its driver and work have stopped. */
void netdev_unregister(struct netdev *nd)
{
    int i;

    for (i = 0; i < nextra && extra[i] != nd; ++i) {
    }
    if (i == nextra)
        return;
    for (; i + 1 < nextra; ++i)
        extra[i] = extra[i + 1];
    extra[--nextra] = 0;
}

/** Return the next service deadline for the stopped idle tick. */
uint32_t netdev_idle_ticks(void)
{
    int i;

    for (i = 0; i < niface; i++) {
        if (ifaces[i].ops->poll) {
            return 1;
        }
    }
    return TICK_HZ;
}
