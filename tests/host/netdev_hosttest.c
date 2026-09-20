/**
 * Host test for the driver table (sys/network/driver/netdev.c).
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
 * The three real drivers are replaced by three models whose verdicts the test
 * sets. netdev.c is compiled against them, so what is under test is the walk
 * itself: the order, the skip, the stop, and the binding.
 *
 * Returns the failed-check count (0 == pass).
 */

#include "lib/kint.h"
#include "network/driver/netdev.h"

/* Declared, not included: the kernel's kint.h and the host's headers disagree
 * about uintptr_t, so this test pulls in no host header and names the one
 * function it needs to say which check failed. */
extern int printf(const char *fmt, ...);

static int fails;

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
    int verdict;
    int next_verdict;
    int inits;
    int stops;
    int txs;
    int polls;
    /* What this model's `link` answers: whether it can say at all, and what
       it says when it can. */
    int can_say;
    struct netdev_link state;
    const char *reason;
};

static struct model m_first, m_second, m_third;
static int refused_card = -1;

/* The order in which anything happened, so a check can say that the stop came
   after the init that failed and that no later driver was ever asked. */
#define TRACE_MAX 32
static char trace[TRACE_MAX];
static int trace_len;

static void note(char c)
{
    if (trace_len < TRACE_MAX - 1) {
        trace[trace_len++] = c;
    }
    trace[trace_len] = '\0';
}

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
static int second_init(struct netdev *nd, int nth)
{
    nd->dev = 0;
    if (nth > 1 || (nth == 1 && m_second.next_verdict == NETDEV_ABSENT)) {
        return NETDEV_ABSENT;
    }
    m_second.inits++;
    note('2');
    return nth == 0 ? m_second.verdict : m_second.next_verdict;
}
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

static void first_stop(void *dev)
{
    (void)dev;
    m_first.stops++;
    note('S');
}
static void second_stop(void *dev)
{
    (void)dev;
    m_second.stops++;
    note('T');
}
static void third_stop(void *dev)
{
    (void)dev;
    m_third.stops++;
    note('U');
}

static int first_tx(void *dev, const uint8_t *d, uint16_t n)
{
    (void)dev;
    (void)d;
    (void)n;
    m_first.txs++;
    return NETDEV_TX_OK;
}
static int second_tx(void *dev, const uint8_t *d, uint16_t n)
{
    (void)dev;
    (void)d;
    (void)n;
    m_second.txs++;
    return NETDEV_TX_DROPPED;
}
static int third_tx(void *dev, const uint8_t *d, uint16_t n)
{
    (void)dev;
    (void)d;
    (void)n;
    m_third.txs++;
    return NETDEV_TX_OK;
}

static void second_poll(void *dev)
{
    (void)dev;
    m_second.polls++;
}

static int first_link(void *dev, struct netdev_link *out)
{
    (void)dev;
    if (!m_first.can_say) {
        return 0;
    }
    *out = m_first.state;
    return 1;
}
static int second_link(void *dev, struct netdev_link *out)
{
    (void)dev;
    if (!m_second.can_say) {
        return 0;
    }
    *out = m_second.state;
    return 1;
}

static const char *first_error(void)
{
    return m_first.reason;
}
static const char *second_error(void)
{
    return m_second.reason;
}
static const char *third_error(void)
{
    return m_third.reason;
}

/* Recorded, so a check that says no driver was called can mean it. */
static int info_calls;

static void nothing(void *dev)
{
    (void)dev;
    info_calls++;
}

/* The names netdev.c links against. The real ones live in the three driver
   files; here they are these models, which is what puts the walk under test
   rather than any card. */
const struct netdev_ops rtl8139_netdev = {"first",    first_init, first_tx,    0,
                                          first_stop, first_link, first_error, nothing};
const struct netdev_ops e1000_netdev = {"second",    second_init, second_tx,    second_poll,
                                        second_stop, second_link, second_error, nothing};
/* The third has no `link`, which is a driver that cannot report one and is
   the case the interface has to answer for as well. */
const struct netdev_ops rtl8168_netdev = {"third",    third_init, third_tx,    0,
                                          third_stop, 0,          third_error, nothing};

#include "network/driver/netdev.c"

/** Put every model back to a state where nothing has happened. */
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
    m_first.reason = "the first card would not start";
    m_second.reason = "the second card would not start";
    m_third.reason = "the third card would not start";
    info_calls = 0;
    trace_len = 0;
    trace[0] = '\0';
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The one address the stack still holds for the machine. */
uint8_t net_mac[6];

/** Supply a single-digit `rach2.nicfail` value when a row requests it. */
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

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The kernel's fill, which the walk reaches and this test does not build. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *q = (unsigned char *)ptr;

    while (n-- > 0) {
        *q++ = (unsigned char)c;
    }
    return ptr;
}

/** Check refusal diagnostics and continued probing after a refused card. */
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
    CHECK(m_second.inits == 2 && m_second.stops == 1 && same(trace, "12T23"),
          "probing continues with the same driver and later drivers");
    CHECK(same(netdev_error(), "the card was refused on the boot line"),
          "successful cards preserve the refusal reason");

    reset(NETDEV_UP, NETDEV_FAILED, NETDEV_ABSENT);
    refused_card = 0;
    CHECK(netdev_probe() == 0, "a refused card and a failed card bind nothing");
    CHECK(same(netdev_error(), m_second.reason), "a later failure supplies its own reason");
}

int main(void)
{
    unsigned char frame[64];
    unsigned i;

    injected_failure();
    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (unsigned char)i;
    }

    /* ---- every card every driver has becomes an interface ---- */
    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a machine with cards brings them up");
    CHECK(netdev_count() == 3, "one interface for each card found");
    CHECK(same(netdev_name(netdev_at(0)), "first"), "the first driver's card is the first");
    CHECK(same(netdev_name(netdev_at(1)), "second"), "the second driver's is next");
    CHECK(same(netdev_name(netdev_at(2)), "third"), "and the third driver's last");
    CHECK(same(trace, "123"), "every driver is asked, in the table's order");
    CHECK(netdev_at(0)->index == 1 && netdev_at(1)->index == 2 && netdev_at(2)->index == 3,
          "and they are numbered from one, as a packet socket binds to them");
    CHECK(same(netdev_at(0)->name, "eth0") && same(netdev_at(1)->name, "eth1") &&
              same(netdev_at(2)->name, "eth2"),
          "and named eth0 upward in the order they were found");

    /* ---- an absent card is stepped over, not stopped ---- */
    reset(NETDEV_ABSENT, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a machine whose first card is absent still brings up the rest");
    CHECK(netdev_count() == 2, "one interface for each card that was there");
    CHECK(same(netdev_name(netdev_first()), "second"), "the next driver's card is the first");
    CHECK(same(trace, "123"), "the absent card is asked once and passed over");
    CHECK(m_first.stops == 0,
          "a card that is not in the machine is not silenced, having never started");

    /* ---- a card that is present and will not start ---- */
    reset(NETDEV_ABSENT, NETDEV_FAILED, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card that will not start does not stop the others");
    CHECK(netdev_count() == 1, "and the machine keeps the one that did start");
    CHECK(same(netdev_name(netdev_first()), "third"), "which is the card that answered");
    CHECK(same(netdev_error(), "the second card would not start"),
          "the reason kept is the card that failed, not the one that was absent");
    CHECK(m_third.inits == 1, "the driver after the one that failed is still asked");
    CHECK(m_second.stops == 1,
          "the card that failed is silenced, because nothing will ever reach it again");
    CHECK(same(trace, "12T3"), "it is silenced before the walk goes on");

    /* ---- a machine with no supported card ---- */
    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_ABSENT);
    CHECK(netdev_probe() == 0, "a machine with no supported card binds nothing");
    CHECK(same(netdev_error(), "no supported network card"),
          "and reports that, rather than the last card's absence");
    CHECK(m_first.stops == 0 && m_second.stops == 0 && m_third.stops == 0,
          "nothing is silenced, because nothing was started");

    /* ---- what the layers above get with nothing bound ---- */
    CHECK(netdev_tx(netdev_first(), frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED,
          "a frame sent with no card is reported dropped, not quietly swallowed");
    netdev_poll();
    netdev_stop(netdev_first());
    netdev_print_info(netdev_first());
    CHECK(m_first.txs == 0 && m_second.txs == 0 && m_third.txs == 0 && m_second.polls == 0 &&
              m_first.stops == 0 && m_second.stops == 0 && m_third.stops == 0 && info_calls == 0,
          "and no driver is called at all");

    /* ---- and with one bound ---- */
    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound again");
    CHECK(netdev_tx(netdev_first(), frame, (uint16_t)sizeof(frame)) == NETDEV_TX_OK,
          "the bound driver's verdict is the one that comes back");
    netdev_poll();
    CHECK(m_first.polls == 0, "a driver with no poll is not polled");
    netdev_stop(netdev_first());
    CHECK(m_first.stops == 1, "the bound driver is the one silenced");

    /* ---- the link, through the interface ---- */
    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound before the link checks");
    {
        struct netdev_link link;

        CHECK(netdev_link(netdev_first(), &link) == 1 && link.up && link.mbps == 100,
              "the bound driver's answer is the one that comes back");
    }

    /* A driver that cannot report a link is not the same as one reporting no
       link, and the interface has to keep them apart. */
    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_UP);
    CHECK(netdev_probe() == 1, "the driver with no link report is bound");
    {
        struct netdev_link link;

        link.up = 42;
        CHECK(netdev_link(netdev_first(), &link) == 0,
              "a driver with no link report says it cannot say");
        CHECK(link.up == 42, "and leaves the caller's answer untouched");
    }

    /* And a machine with nothing bound at all. */
    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_ABSENT);
    CHECK(netdev_probe() == 0, "nothing is bound");
    {
        struct netdev_link link;

        link.up = 42;
        CHECK(netdev_link(netdev_first(), &link) == 0, "a machine with no card cannot say either");
        CHECK(link.up == 42, "and leaves the caller's answer untouched");
    }

    /* ---- what IFF_RUNNING is derived from ---- */
    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound before the running checks");
    m_first.state.up = 1;
    CHECK(netdev_running(netdev_first()) == 1, "a card reporting a live link is running");
    m_first.state.up = 0;
    CHECK(netdev_running(netdev_first()) == 0,
          "a card reporting a dead link is not running, which is the whole point: "
          "a bound card and a working one are different things");

    /* The card that cannot say. Nothing in this tree returns 0 from `link`,
       so this branch exists for a driver that has no way to read one, and the
       third model is that driver. Without a check here it would be reachable
       through the interface and proved by nothing. */
    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_UP);
    CHECK(netdev_probe() == 1, "the driver with no link report is bound");
    CHECK(netdev_running(netdev_first()) == 1,
          "a card that cannot report a link is reported running, which is the most "
          "that can be said of it and what the flag meant before");

    /* And a card that could report one but declines to. */
    reset(NETDEV_UP, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "a card is bound");
    m_first.can_say = 0;
    CHECK(netdev_running(netdev_first()) == 1,
          "a card that declines to answer is treated the same way");

    reset(NETDEV_ABSENT, NETDEV_ABSENT, NETDEV_ABSENT);
    CHECK(netdev_probe() == 0, "nothing is bound");
    CHECK(netdev_running(netdev_first()) == 0, "a machine with no card is not running");

    /* A driver whose transmit refuses: the refusal reaches the caller
       unchanged, which is what lets the interface count it. */
    reset(NETDEV_ABSENT, NETDEV_UP, NETDEV_UP);
    CHECK(netdev_probe() == 1, "the second card is bound");
    CHECK(netdev_tx(netdev_first(), frame, (uint16_t)sizeof(frame)) == NETDEV_TX_DROPPED,
          "a driver that refuses a frame has its refusal passed up");
    netdev_poll();
    CHECK(m_second.polls == 1, "a driver that needs polling is polled");

    if (!fails) {
        printf("netdev-hosttest: result: pass\n");
    }
    return fails;
}
