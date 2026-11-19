/**
 * Test the walk of the driver table of sys/network/driver/netdev.c with
 * model drivers in place of the real ones.
 *
 * host-deps:
 *
 * The table is small and it decides three things no single driver can be
 * asked about: which card a machine with more than one gets, what happens to
 * a card that is present and will not start, and what a machine with no
 * supported card reports. A boot can show the first of those and only on a
 * machine that has two cards; the other two need drivers that misbehave on
 * purpose, which is what this file supplies.
 *
 * The table's drivers are replaced by models: the first three by three
 * whose verdicts the test sets, and the virtio driver, its last, by one with
 * no card. netdev.c is compiled against them, so what is under test is
 * the walk itself: the order, the skip, the stop, and the binding.
 *
 * A card's hand-over to another driver and back (`netdev_release`,
 * `netdev_reclaim` and their files in /sys) is run against stand-ins for
 * PCI, /sys and the stack that record what was asked of them.
 *
 * The test exits with the number of failed checks, at most 255.
 */

#include "lib/kint.h"
#include "lib/kerrno.h"
#include "fs/sysfs.h"
#include "fs/synthfs.h"
#include "pci.h"
#include "network/driver/netdev.h"

/* Declared, not included: the kernel's kint.h and the host's headers disagree
 * about uintptr_t, so this test pulls in no host header and names the one
 * function it needs to say which check failed. */
extern int printf(const char *fmt, ...);

static int fails;

/* The tick count netdev.c keeps a carrier reading by; the test moves it. */
volatile uint64_t kernel_ticks;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/* ------------------------------------------------------------------ */
/* Three models in place of the three drivers.                         */
/* ------------------------------------------------------------------ */

/* What each model's `init` will answer, and what it recorded. */
struct model {
    int verdict;      /* NETDEV_UP, NETDEV_FAILED or NETDEV_ABSENT, card 0 */
    int next_verdict; /* the same for card 1, which only the second model has */
    int inits;
    int stops;
    int txs;
    int polls;
    /* What this model's `link` answers: whether it can say at all, and what
       it says when it can. */
    int can_say;
    struct netdev_link state;
    const char *reason;
    int pci_key;   /* the function card 0 is, or -1, as a driver sets it */
    int fail_next; /* the next inits of card 0 that fail before its verdict */
};

static struct model m_first, m_second, m_third;
static int refused_card = -1; /* the card rach2.nicfail names, or -1 */

/* The order in which anything happened, so a check can say that the stop came
   after the init that failed and that no later driver was ever asked: '0' for
   the virtio model's init, '1' to '3' for the three models', and 'S', 'T' and
   'U' for their stops. */
#define TRACE_MAX 32
static char trace[TRACE_MAX];
static int trace_len;

/** Append `c` to `trace`, which keeps its last byte for the terminator. */
static void note(char c)
{
    if (trace_len < TRACE_MAX - 1) {
        trace[trace_len++] = c;
    }
    trace[trace_len] = '\0';
}

/** Probe the first model's card `nth`: card 0 gets `m_first.verdict`, any
 * other is absent. */
static int first_init(struct netdev *nd, int nth)
{
    nd->dev = 0; /* this model has one card */
    if (nth != 0) {
        return NETDEV_ABSENT;
    }
    m_first.inits++;
    note('1');
    return m_first.verdict;
}
/** Probe the second model's card `nth`: card 0 gets `verdict`, card 1
 * `next_verdict`, and any other is absent. */
static int second_init(struct netdev *nd, int nth)
{
    nd->dev = 0;
    if (nth > 1 || (nth == 1 && m_second.next_verdict == NETDEV_ABSENT)) {
        return NETDEV_ABSENT;
    }
    m_second.inits++;
    note('2');
    if (nth == 0) {
        nd->pci_key = m_second.pci_key;
        if (m_second.fail_next > 0) {
            m_second.fail_next--;
            return NETDEV_FAILED;
        }
    }
    return nth == 0 ? m_second.verdict : m_second.next_verdict;
}
/** Probe the third model's card `nth`: card 0 gets `m_third.verdict`, any
 * other is absent. */
static int third_init(struct netdev *nd, int nth)
{
    nd->dev = 0; /* this model has one card */
    if (nth != 0) {
        return NETDEV_ABSENT;
    }
    m_third.inits++;
    note('3');
    return m_third.verdict;
}

/** Stop the first model's card and note 'S'. */
static void first_stop(void *dev)
{
    (void)dev;
    m_first.stops++;
    note('S');
}
/** Stop the second model's card and note 'T'. */
static void second_stop(void *dev)
{
    (void)dev;
    m_second.stops++;
    note('T');
}
/** Stop the third model's card and note 'U'. */
static void third_stop(void *dev)
{
    (void)dev;
    m_third.stops++;
    note('U');
}

/** Count a frame sent by the first model and return NETDEV_TX_OK. */
static int first_tx(void *dev, const uint8_t *d, uint16_t n)
{
    (void)dev;
    (void)d;
    (void)n;
    m_first.txs++;
    return NETDEV_TX_OK;
}
/** Count a frame sent by the second model and drop it: NETDEV_TX_DROPPED. */
static int second_tx(void *dev, const uint8_t *d, uint16_t n)
{
    (void)dev;
    (void)d;
    (void)n;
    m_second.txs++;
    return NETDEV_TX_DROPPED;
}
/** Count a frame sent by the third model and return NETDEV_TX_OK. */
static int third_tx(void *dev, const uint8_t *d, uint16_t n)
{
    (void)dev;
    (void)d;
    (void)n;
    m_third.txs++;
    return NETDEV_TX_OK;
}

/** Count a poll of the second model, the one driver with a poll. */
static void second_poll(void *dev)
{
    (void)dev;
    m_second.polls++;
}

/** Give the first model's link state and return 1, or return 0 when
 * `can_say` is clear. */
static int first_link(void *dev, struct netdev_link *out)
{
    (void)dev;
    if (!m_first.can_say) {
        return 0;
    }
    *out = m_first.state;
    return 1;
}
/** Give the second model's link state and return 1, or return 0 when
 * `can_say` is clear. */
static int second_link(void *dev, struct netdev_link *out)
{
    (void)dev;
    if (!m_second.can_say) {
        return 0;
    }
    *out = m_second.state;
    return 1;
}

/** Return the first model's reason for a card that did not start. */
static const char *first_error(void)
{
    return m_first.reason;
}
/** Return the second model's reason for a card that did not start. */
static const char *second_error(void)
{
    return m_second.reason;
}
/** Return the third model's reason for a card that did not start. */
static const char *third_error(void)
{
    return m_third.reason;
}

/* Recorded, so a check that says no driver was called can mean it. */
static int info_calls;

/** Print a card's details: nothing, but count the call. */
static void nothing(void *dev)
{
    (void)dev;
    info_calls++;
}

/** Probe the virtio model, which has no card: the table's last driver is
 * asked after the three, notes '0' and answers NETDEV_ABSENT. */
static int virtio_init(struct netdev *nd, int nth)
{
    (void)nd;
    (void)nth;
    note('0');
    return NETDEV_ABSENT;
}

/* The names netdev.c links against. The real ones live in the four driver
   files; here they are these models, which is what puts the walk under test
   rather than any card. The virtio driver, last in the table, is a model
   with no card, so the three before it are the ones the rows set. */
const struct netdev_ops virtio_net_netdev = {"virtio", virtio_init, 0, 0, 0, 0, 0, 0};
const struct netdev_ops rtl8139_netdev = {"first",    first_init, first_tx,    0,
                                          first_stop, first_link, first_error, nothing};
const struct netdev_ops e1000_netdev = {"second",    second_init, second_tx,    second_poll,
                                        second_stop, second_link, second_error, nothing};
/* The third has no `link`, which is a driver that cannot report one and is
   the case the interface has to answer for as well. */
const struct netdev_ops rtl8168_netdev = {"third",    third_init, third_tx,    0,
                                          third_stop, 0,          third_error, nothing};

#include "network/driver/netdev.c"

/** Put every model back to a state where nothing has happened, with `a`, `b`
 * and `c` as the verdicts of the three models' first cards. */
static void reset(int a, int b, int c)
{
    m_first.verdict = a;
    m_second.verdict = b;
    m_third.verdict = c;
    m_second.next_verdict = NETDEV_ABSENT;
    refused_card = -1;
    m_first.inits = m_second.inits = m_third.inits = 0;
    m_first.stops = m_second.stops = m_third.stops = 0;
    m_first.txs = m_second.txs = m_third.txs = 0;
    m_first.polls = m_second.polls = m_third.polls = 0;
    m_first.can_say = 1;
    m_second.can_say = 1;
    m_third.can_say = 1;
    m_first.state.up = 1;
    m_first.state.mbps = 100;
    m_first.state.full_duplex = 1;
    m_second.state.up = 0;
    m_second.state.mbps = 0;
    m_second.state.full_duplex = 0;
    m_first.pci_key = m_second.pci_key = m_third.pci_key = -1;
    m_first.fail_next = m_second.fail_next = m_third.fail_next = 0;
    m_first.reason = "the first card would not start";
    m_second.reason = "the second card would not start";
    m_third.reason = "the third card would not start";
    info_calls = 0;
    trace_len = 0;
    trace[0] = '\0';
}

/** Return 1 when the strings `a` and `b` are equal, else 0. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The first interface's station address, which netdev_probe copies in. */
uint8_t net_mac[6];

/** Give `rach2.nicfail` the digit `refused_card` when it is set, as the boot
 * line would; return 1 then, and 0 with `out` empty for any other key. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    if (same(key, "rach2.nicfail") && refused_card >= 0 && outlen >= 2) {
        out[0] = (char)('0' + refused_card);
        out[1] = '\0';
        return 1;
    }
    if (outlen > 0) {
        out[0] = '\0';
    }
    return 0;
}

/** Discard a kernel log record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Fill `n` bytes at `ptr` with `c`: the kernel's k_memset, which the walk
 * calls and this test does not build. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *q = (unsigned char *)ptr;

    while (n-- > 0) {
        *q++ = (unsigned char)c;
    }
    return ptr;
}

/* ------------------------------------------------------------------ */
/* The kernel around a card's hand-over: PCI, /sys and the stack.      */
/* ------------------------------------------------------------------ */

/* What the hand-over did to the kernel around the card, which the rows
   read: the function's owner, which a row sets to stand for vfio-pci, and
   its power state; the last unclaim, bind, unbind and uevent; the last
   up or down set on an interface; the microseconds waited; and the
   brackets of net_lock still open. */
static struct {
    const char *owner;
    int power;
    int unclaims, unclaim_devfn;
    const char *unclaim_owner;
    int binds, bind_key;
    const char *bind_driver;
    int unbinds, unbind_key;
    const char *uevent;
    int set_ups, last_up;
    uint32_t waited;
    int locks;
    int dirs, attrs;
    const char *override; /* the function's driver_override, "" for none, NULL ungrouped */
    int bind_error;       /* what the next sysfs_pci_bind answers */
    int dir_parent[8];
    const char *dir_name[8];
    const struct sysfs_attr *attr[2][2]; /* [driver][bind, unbind] */
} k;

/** The kernel's k_strlen. */
size_t k_strlen(const char *str)
{
    size_t n = 0;

    while (str[n]) {
        n++;
    }
    return n;
}

/** Count the wait instead of waiting. */
void k_udelay(uint32_t usec)
{
    k.waited += usec;
}

/** Open a bracket of the stack's lock, counted. */
uint32_t net_lock(void)
{
    k.locks++;
    return 0;
}

/** Close a bracket of the stack's lock, counted. */
void net_unlock(uint32_t flags)
{
    (void)flags;
    k.locks--;
}

/** Record an interface set up or down. */
int netconf_set_up(struct netdev *nd, int up)
{
    (void)nd;
    k.set_ups++;
    k.last_up = up;
    return 0;
}

/* The withdrawal's calls into the stack (`netdev_withdraw`), which no row
 * makes: no route, address or neighbour to forget. */

/** No route to give. */
int netdev_route_at(int n, struct netdev_rt *out)
{
    (void)n;
    (void)out;
    return 0;
}

/** No route to delete. */
int netdev_route_del(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw)
{
    (void)nd;
    (void)dst;
    (void)mask;
    (void)gw;
    return -ESRCH;
}

/** No address to clear. */
int netconf_set_address(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    (void)nd;
    (void)ip;
    (void)mask;
    return 0;
}

/** No neighbour to forget. */
void arp_forget_dev(struct netdev *nd)
{
    (void)nd;
}

/** No neighbour to list. */
int arp_snapshot(struct arp_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** No neighbour to forget. */
int arp_forget(struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return -ENOENT;
}

/** No IPv6 state to take down. */
void nd6_if_down(struct netdev *nd)
{
    (void)nd;
}

/** No IPv6 neighbour to list. */
int nd6_snapshot(struct nd6_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** No IPv6 neighbour to forget. */
int nd6_forget(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return -ENOENT;
}

/** No IPv6 route to forget. */
void route6_forget(struct netdev *nd, int proto)
{
    (void)nd;
    (void)proto;
}

/** No IPv6 address to forget. */
void ip6_forget_dev(struct netdev *nd)
{
    (void)nd;
}

/** No packet socket bound. */
void packet_dev_gone(int ifindex)
{
    (void)ifindex;
}

/** The function's owner, as the row set it. */
const char *pci_owner(uint8_t bus, uint8_t devfn)
{
    (void)bus;
    (void)devfn;
    return k.owner;
}

/** Record the function given up. */
void pci_unclaim(uint8_t bus, uint8_t devfn, const char *owner)
{
    k.unclaims++;
    k.unclaim_devfn = bus << 8 | devfn;
    k.unclaim_owner = owner;
}

/** The function's power state, as the row set it. */
int pci_power_state(const pci_device_t *dev)
{
    (void)dev;
    return k.power;
}

/** Move the function to `state`. */
int pci_set_power_state(const pci_device_t *dev, int state)
{
    (void)dev;
    k.power = state;
    return 0;
}

/** Record the function shown bound, or answer the error a row set. */
int sysfs_pci_bind(int key, const char *driver)
{
    if (k.bind_error) {
        int r = k.bind_error;

        k.bind_error = 0;
        return r;
    }
    k.binds++;
    k.bind_key = key;
    k.bind_driver = driver;
    return 0;
}

/** Record the function shown unbound. */
void sysfs_pci_unbind(int key)
{
    k.unbinds++;
    k.unbind_key = key;
}

/** Record the uevent sent. */
int32_t sysfs_pci_uevent(int key, const char *action)
{
    (void)key;
    k.uevent = action;
    return 0;
}

/** /sys/bus/pci's anchor: handle 1. */
int sysfs_anchor(int node, int key)
{
    (void)key;
    return node == SYSFS_BUS_PCI ? 1 : -ENOENT;
}

/** VFIO's driver_override of the function, as group.c compares it: the
 * stored text, so that "(null)" names a driver. */
int vfio_override_allows(int key, const char *driver)
{
    (void)key;
    return !k.override || !k.override[0] || same(k.override, driver);
}

/** The directory `name` made below `dir`, or 0. */
int sysfs_node_child(int dir, const char *name, int len)
{
    int i;

    (void)len;
    for (i = 0; i < k.dirs; i++) {
        if (k.dir_parent[i] == dir && same(k.dir_name[i], name)) {
            return 10 + i;
        }
    }
    return 0;
}

/** Make a directory: handles from 10. */
int sysfs_add_dir(int parent, const char *name)
{
    if (parent <= 0 || k.dirs == 8) {
        return parent <= 0 ? -ENOENT : -ENOSPC;
    }
    k.dir_parent[k.dirs] = parent;
    k.dir_name[k.dirs] = name;
    return 10 + k.dirs++;
}

/** Keep each driver's bind and unbind, so that a row writes them. */
int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)obj;
    if (arg >= 0 && arg < 2) {
        k.attr[arg][same(attr->name, "bind") ? 0 : 1] = attr;
    }
    return 100 + k.attrs++;
}

/** Write `text` to the attribute `which`, 0 bind or 1 unbind, of driver
 * `drv`, 0 e1000 or 1 r8169, as a write of the file does. */
static int32_t write_attr(int drv, int which, const char *text)
{
    return k.attr[drv][which]->store(0, drv, text, (uint32_t)k_strlen(text));
}

/* The function of the second model's card, as a write of /sys gives it. */
static const char name[] = "0000:00:03.0\n";

static void check_takeback(struct netdev *nd, int txs);

/**
 * Give the second model's card, an e1000 at 00:03.0, to another driver
 * through /sys and take it back: the interface stays, down, without a
 * carrier and dropping frames, while the card is away; the function is
 * given up and shown unbound; a function another driver holds is not
 * taken; the card is brought to D0 before its bring-up; a card that fails
 * its first bring-up is tried once more; and one that fails both stays
 * away.
 */
static void check_handover(void)
{
    unsigned char frame[64];
    struct netdev *nd;
    struct netdev_link link;
    int index, txs;

    k_memset(&k, 0, sizeof(k));
    k_memset(frame, 0, sizeof(frame));
    reset(NETDEV_UP, NETDEV_UP, NETDEV_ABSENT);
    m_second.pci_key = 3 << 3;
    m_second.state.up = 1;
    CHECK(netdev_probe() == 1 && netdev_count() == 2, "two cards come up for the hand-over");
    nd = netdev_at(1);
    index = nd->index;
    CHECK(netdev_release(netdev_at(0)) == -ENODEV, "a card that is no PCI function is not let go");

    netdev_sys_start();
    CHECK(k.dirs == 3 && k.attrs == 4 && k.attr[0][0] && k.attr[0][1] && k.attr[1][0] &&
              k.attr[1][1],
          "drivers/, e1000/ and r8169/ are made, each driver with bind and unbind");
    CHECK(k.binds == 1 && k.bind_key == (3 << 3) && same(k.bind_driver, "e1000"),
          "the e1000's card is shown bound to e1000 and the r8169's to nothing");

    CHECK(write_attr(1, 1, name) == -ENODEV, "r8169's unbind does not take the e1000's card");
    CHECK(write_attr(0, 1, "0000:00:20.0") == -ENODEV && write_attr(0, 1, "00:03.0") == -ENODEV &&
              write_attr(0, 1, "0000:00:03.8") == -ENODEV,
          "a name that is no function's is refused");
    CHECK(m_second.stops == 0, "and nothing is stopped by a refused write");

    txs = m_second.txs;
    CHECK(write_attr(0, 1, name) == (int32_t)(sizeof(name) - 1), "unbind takes the name");
    CHECK(m_second.stops == 1, "the driver stops the card");
    CHECK(k.set_ups == 1 && k.last_up == 0, "the interface is set down");
    CHECK(k.unclaims == 1 && k.unclaim_devfn == (3 << 3) && same(k.unclaim_owner, "kernel"),
          "the function is given up");
    CHECK(k.unbinds == 1 && k.unbind_key == (3 << 3) && same(k.uevent, "unbind"),
          "and shown unbound, with its uevent");
    CHECK(netdev_tx(nd, frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED && m_second.txs == txs,
          "a frame sent while the card is away is dropped and the driver not called");
    link.up = 42;
    CHECK(netdev_link(nd, &link) == 1 && link.up == 0, "the interface has no carrier");
    CHECK(nd == netdev_at(1) && nd->index == index && same(nd->name, "eth1"),
          "the interface stays, with its name and index");
    CHECK(write_attr(0, 1, name) == -ENODEV, "a card already let go is not let go again");
    CHECK(nd->ops->set_up && nd->ops->set_up(nd->dev, 1) == -ENODEV &&
              nd->ops->set_up(nd->dev, 0) == 0,
          "the interface may not be set up while its card is away, and may go down");
    CHECK(k.locks == 0, "every bracket of the stack's lock is closed");
    check_takeback(nd, txs);
}

/**
 * Take back the card `nd` that check_handover let go, whose driver had
 * sent `txs` frames: refused while another driver holds the function,
 * brought to D0 first, tried twice, and left away when both tries fail.
 */
static void check_takeback(struct netdev *nd, int txs)
{
    unsigned char frame[64];

    k_memset(frame, 0, sizeof(frame));
    k.owner = "vfio-pci";
    CHECK(netdev_reclaim(nd, "e1000") == -EBUSY, "a function another driver holds is busy");
    CHECK(write_attr(0, 0, name) == -ENODEV,
          "and bind answers ENODEV for it, as Linux's bind_store for a device with a driver");
    CHECK(m_second.inits == 1, "and the driver does not touch it");

    k.owner = 0;
    k.override = "vfio-pci";
    CHECK(write_attr(0, 0, name) == -ENODEV && m_second.inits == 1,
          "bind is ENODEV while driver_override names another driver");
    k.override = "(null)";
    CHECK(write_attr(0, 0, name) == -ENODEV && m_second.inits == 1,
          "and while it holds the text (null), which is not the override's absence");
    k.override = "";
    k.bind_error = -ENOSPC;
    k.unclaims = 0;
    CHECK(write_attr(0, 0, name) == -ENOSPC && m_second.inits == 2 && m_second.stops == 2 &&
              k.unclaims == 1,
          "a bind /sys cannot show stops the card again and gives the function up");
    CHECK(netdev_tx(nd, frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED && m_second.txs == txs,
          "and leaves it away");
    k.override = "e1000";
    k.power = 3;
    k.waited = 0;
    k.binds = 0;
    CHECK(write_attr(0, 0, name) == (int32_t)(sizeof(name) - 1), "bind takes the name");
    CHECK(k.power == 0 && k.waited >= PCI_PM_D3HOT_WAIT * 1000u,
          "a card left in D3hot is brought to D0, with the wait from D3hot, first");
    CHECK(m_second.inits == 3, "the driver brings the card up again");
    CHECK(netdev_tx(nd, frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED &&
              m_second.txs == txs + 1,
          "a frame sent after is the driver's again");
    CHECK(k.binds == 1 && same(k.bind_driver, "e1000") && same(k.uevent, "bind"),
          "the function is shown bound to e1000, with its uevent");
    CHECK(k.set_ups == 1, "the interface stays down until it is set up");
    CHECK(write_attr(0, 0, name) == -ENODEV, "a card the driver has is not taken again");

    CHECK(write_attr(0, 1, name) > 0, "the card is let go a second time");
    m_second.fail_next = 1;
    k.waited = 0;
    k.unclaims = 0;
    CHECK(netdev_reclaim(nd, 0) == 0, "a card whose first bring-up fails is taken back");
    CHECK(m_second.inits == 5 && m_second.stops == 4 && k.unclaims == 1 && k.waited == 100000u,
          "after a stop, the function given up and 100 ms, the second bring-up starts it");

    CHECK(write_attr(0, 1, name) > 0, "the card is let go a third time");
    m_second.fail_next = 2;
    k.unclaims = 0;
    CHECK(netdev_reclaim(nd, "e1000") == -EIO, "a card that fails both bring-ups is EIO");
    CHECK(m_second.inits == 7 && k.unclaims == 2, "each failed bring-up gives the function up");
    txs = m_second.txs;
    CHECK(netdev_tx(nd, frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED && m_second.txs == txs,
          "and the card stays away");
    CHECK(nd->pci_key == (3 << 3), "keeping its function, so that a later bind finds it");
    CHECK(write_attr(0, 0, name) > 0, "a later bind takes it back");
    CHECK(k.locks == 0, "every bracket of the stack's lock is closed at the end");
}

/**
 * Check a card refused by `rach2.nicfail`: it is stopped and its reason
 * kept; the walk goes on to the same driver's next card and the drivers
 * after it, and the reason stays when they come up; and a later card that
 * fails gives its own reason.
 */
static void injected_failure(void)
{
    reset(NETDEV_ABSENT, NETDEV_UP, NETDEV_ABSENT);
    refused_card = 0;
    m_second.reason = "no e1000 present";
    CHECK(netdev_probe() == 0, "the only card is refused");
    CHECK(m_second.stops == 1, "the refused card is stopped");
    CHECK(same(netdev_error(), "the card was refused on the boot line"),
          "injected refusal reason is retained");

    reset(NETDEV_ABSENT, NETDEV_UP, NETDEV_UP);
    m_second.next_verdict = NETDEV_UP;
    refused_card = 0;
    CHECK(netdev_probe() == 1 && netdev_count() == 2,
          "cards after a refusal still become interfaces");
    CHECK(m_second.inits == 2 && m_second.stops == 1 && same(trace, "12T230"),
          "probing continues with the same driver and later drivers");
    CHECK(same(netdev_error(), "the card was refused on the boot line"),
          "successful cards preserve the refusal reason");

    reset(NETDEV_UP, NETDEV_FAILED, NETDEV_ABSENT);
    refused_card = 0;
    CHECK(netdev_probe() == 0, "a refused card and a failed card bind nothing");
    CHECK(same(netdev_error(), m_second.reason), "a later failure supplies its own reason");
}

/** A card's link decides whether it runs; a live link stands for its
 * window without a reading of the card, across the tick count's wrap; a
 * dead link is read at every look; and a change the driver reports makes
 * the next look read the card. */
static void check_carrier(void)
{
    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound before the running checks");
    m_first.state.up = 1;
    CHECK(netdev_running(netdev_first()) == 1, "a card reporting a live link is running");
    m_first.state.up = 0;
    netdev_carrier_changed(netdev_first());
    CHECK(netdev_running(netdev_first()) == 0,
          "a card reporting a dead link is not running, which is the whole point: "
          "a bound card and a working one are different things");

    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound before the carrier checks");
    kernel_ticks = 0xFFFFFF00u;
    CHECK(netdev_running(netdev_first()) == 1, "a live link is read from the card");
    m_first.state.up = 0;
    kernel_ticks += NETDEV_CARRIER_TICKS - 1;
    CHECK(netdev_running(netdev_first()) == 1,
          "a live link stands for its window without a reading, across the low "
          "half's wrap, as Linux's carrier stands between its watchdog's runs");
    kernel_ticks++;
    CHECK(netdev_running(netdev_first()) == 0, "a link that dropped is read at the window's end");
    m_first.state.up = 1;
    CHECK(netdev_running(netdev_first()) == 1,
          "a dead link is read at every look, so one coming back is seen at once");
    m_first.state.up = 0;
    netdev_carrier_changed(netdev_first());
    CHECK(netdev_running(netdev_first()) == 0,
          "a change the driver reports makes the next look read the card");
    kernel_ticks = 0;
}

/**
 * Run injected_failure, then the rows of the walk: every card of every
 * driver becomes an interface, in the table's order, numbered from 1 and
 * named from eth0; an absent card is passed over and not stopped; a card that
 * is present and does not start is stopped before the walk goes on, and its
 * reason is kept; and a machine with no supported card says so.
 *
 * With no card bound a frame is dropped and no driver is called; with one,
 * its transmit, poll and stop are the ones called, and a drop it reports
 * reaches the caller unchanged, which lets the interface count it.
 *
 * netdev_link keeps apart a driver that reports no link and one that cannot
 * report one: for the third model, which has no `link`, or with nothing
 * bound, it returns 0 and leaves the caller's answer alone. netdev_running,
 * the source of IFF_RUNNING, follows the link a card reports, and a card
 * that cannot report one, or does not, is running. Every driver of the table
 * has a `link`, so only the third model reaches that branch.
 *
 * Returns the number of failed checks, at most 255.
 */
int main(void)
{
    unsigned char frame[64];
    unsigned i;

    injected_failure();
    check_handover();
    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (unsigned char)i;
    }

    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a machine with cards brings them up");
    CHECK(netdev_count() == 3, "one interface for each card found");
    CHECK(same(netdev_name(netdev_at(0)), "first"), "the first driver's card is the first");
    CHECK(same(netdev_name(netdev_at(1)), "second"), "the second driver's is next");
    CHECK(same(netdev_name(netdev_at(2)), "third"), "and the third driver's last");
    CHECK(same(trace, "1230"), "every driver is asked, in the table's order");
    CHECK(netdev_at(0)->index == 1 && netdev_at(1)->index == 2 && netdev_at(2)->index == 3,
          "and they are numbered from one, as a packet socket binds to them");
    CHECK(same(netdev_at(0)->name, "eth0") && same(netdev_at(1)->name, "eth1") &&
              same(netdev_at(2)->name, "eth2"),
          "and named eth0 upward in the order they were found");

    reset(NETDEV_ABSENT, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a machine whose first card is absent still brings up the rest");
    CHECK(netdev_count() == 2, "one interface for each card that was there");
    CHECK(same(netdev_name(netdev_first()), "second"), "the next driver's card is the first");
    CHECK(same(trace, "1230"), "the absent card is asked once and passed over");
    CHECK(m_first.stops == 0,
          "a card that is not in the machine is not silenced, having never started");

    reset(NETDEV_ABSENT, NETDEV_FAILED, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card that will not start does not stop the others");
    CHECK(netdev_count() == 1, "and the machine keeps the one that did start");
    CHECK(same(netdev_name(netdev_first()), "third"), "which is the card that answered");
    CHECK(same(netdev_error(), "the second card would not start"),
          "the reason kept is the card that failed, not the one that was absent");
    CHECK(m_third.inits == 1, "the driver after the one that failed is still asked");
    CHECK(m_second.stops == 1,
          "the card that failed is silenced, because nothing will ever reach it again");
    CHECK(same(trace, "12T30"), "it is silenced before the walk goes on");

    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_ABSENT);
    CHECK(netdev_probe() == 0, "a machine with no supported card binds nothing");
    CHECK(same(netdev_error(), "no supported network card"),
          "and reports that, rather than the last card's absence");
    CHECK(m_first.stops == 0 && m_second.stops == 0 && m_third.stops == 0,
          "nothing is silenced, because nothing was started");

    CHECK(netdev_tx(netdev_first(), frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED,
          "a frame sent with no card is reported dropped, not quietly swallowed");
    netdev_poll();
    netdev_stop(netdev_first());
    netdev_print_info(netdev_first());
    CHECK(m_first.txs == 0 && m_second.txs == 0 && m_third.txs == 0 && m_second.polls == 0 &&
              m_first.stops == 0 && m_second.stops == 0 && m_third.stops == 0 && info_calls == 0,
          "and no driver is called at all");

    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound again");
    CHECK(netdev_tx(netdev_first(), frame, (uint16_t)sizeof(frame)) == NETDEV_TX_OK,
          "the bound driver's verdict is the one that comes back");
    netdev_poll();
    CHECK(m_first.polls == 0, "a driver with no poll is not polled");
    netdev_stop(netdev_first());
    CHECK(m_first.stops == 1, "the bound driver is the one silenced");

    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound before the link checks");
    {
        struct netdev_link link;

        CHECK(netdev_link(netdev_first(), &link) == 1 && link.up && link.mbps == 100,
              "the bound driver's answer is the one that comes back");
    }

    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_UP);
    CHECK(netdev_probe() == 1, "the driver with no link report is bound");
    {
        struct netdev_link link;

        link.up = 42;
        CHECK(netdev_link(netdev_first(), &link) == 0,
              "a driver with no link report says it cannot say");
        CHECK(link.up == 42, "and leaves the caller's answer untouched");
    }

    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_ABSENT);
    CHECK(netdev_probe() == 0, "nothing is bound");
    {
        struct netdev_link link;

        link.up = 42;
        CHECK(netdev_link(netdev_first(), &link) == 0, "a machine with no card cannot say either");
        CHECK(link.up == 42, "and leaves the caller's answer untouched");
    }

    check_carrier();

    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_UP);
    CHECK(netdev_probe() == 1, "the driver with no link report is bound");
    CHECK(netdev_running(netdev_first()) == 1,
          "a card that cannot report a link is reported running, which is the most "
          "that can be said of it and what the flag meant before");

    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound");
    m_first.can_say = 0;
    CHECK(netdev_running(netdev_first()) == 1,
          "a card that declines to answer is treated the same way");

    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_ABSENT);
    CHECK(netdev_probe() == 0, "nothing is bound");
    CHECK(netdev_running(netdev_first()) == 0, "a machine with no card is not running");

    reset(NETDEV_ABSENT, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "the second card is bound");
    CHECK(netdev_tx(netdev_first(), frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED,
          "a driver that refuses a frame has its refusal passed up");
    netdev_poll();
    CHECK(m_second.polls == 1, "a driver that needs polling is polled");

    if (!fails) {
        printf("netdev-hosttest: result: pass\n");
    }
    return fails > 255 ? 255 : fails;
}
