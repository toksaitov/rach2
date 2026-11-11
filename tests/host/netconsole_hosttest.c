/*
 * netconsole_hosttest.c: drive the kernel log's network console, netconsole.c,
 * on the host, with the Ethernet layer replaced by a recorder.
 *
 * host-deps:
 *
 * netconsole.c is compiled natively and driven through the same entry points
 * the kernel uses. This covers the cases the QEMU capture (`tests/run
 * netconsole`) cannot produce on demand: a ring that fills and drops, a
 * payload that fills exactly, and above all a driver that prints an error
 * from inside the transmit it is performing, a recursion netconsole.c must
 * not enter and a QEMU boot without faults does not cause.
 *
 * The file is included, not linked, so the test can put its statics back to
 * their initial state between cases; the kernel has one boot per run and needs
 * no such thing.
 */

extern int printf(const char *fmt, ...);

/* The kernel headers this pulls in define uint32_t themselves, so nothing
   from the host's stdint may be included alongside them. */
#include "../../rach2/sys/network/netconsole.h"
#include "../../rach2/sys/network/driver/netdev.h"
#include "../../rach2/sys/network/ip6/ip6.h"
#include "../../rach2/sys/mm/kheap.h"
#include "../../rach2/sys/panic.h"
#include "../../rach2/sys/timer.h"

/* ------------------------------------------------------------------ */
/* The stubs netconsole links against.                                 */
/* ------------------------------------------------------------------ */

#define MAX_FRAMES 64

static unsigned char frame_data[MAX_FRAMES][NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX];
static unsigned short frame_len[MAX_FRAMES];
static unsigned short frame_type[MAX_FRAMES];
static unsigned char frame_dst[MAX_FRAMES][6];
static unsigned frame_count; /* frames sent; the first MAX_FRAMES are kept */

/* When set, the recorder prints from inside the transmit, exactly as a driver
   reporting an error would, and that print re-enters netconsole_putc. */
static int tx_prints;
static unsigned tx_depth;     /* transmits under way */
static unsigned tx_max_depth; /* the most transmits under way at once */

static void log_putc(char c);

/** Discard a kernel log record; no row reads netconsole's own report. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The interface a frame leaves by, which this model does not look
 * inside. */
struct netdev;

/* The one interface netdev_first gives, `eth0`; the target rows set its
 * IPv4 address, the only field netconsole.c reads here. */
static struct netdev the_iface;

/* Whether the card says its link is up, and the frame after which it goes
 * down, which models a link lost in the middle of a drain (0 for never). */
static int link_down;
static int link_lost_after;
/* A card that drops every frame with its link up, and the frames offered. */
static int refuse_all;
static unsigned tx_attempts;

/** Return the model's one interface. */
struct netdev *netdev_first(void)
{
    return &the_iface;
}

/** Report the link up unless `link_down` is set. */
int netdev_running(const struct netdev *nd)
{
    return nd && !link_down;
}

/**
 * Record a frame sent, or drop it when the link is down or `refuse_all` is
 * set. A recorded frame prints from inside the transmit when `tx_prints` is
 * set, and takes the link down when it is frame `link_lost_after`. Returns
 * NETDEV_TX_OK for a recorded frame, NETDEV_TX_DROPPED for a dropped frame.
 */
int ethernet_tx(struct netdev *out, const unsigned char *dst, unsigned short type,
                const unsigned char *payload, unsigned short plen)
{
    unsigned i;

    (void)out;
    tx_attempts++;
    if (link_down || refuse_all) {
        return NETDEV_TX_DROPPED;
    }
    tx_depth++;
    if (tx_depth > tx_max_depth) {
        tx_max_depth = tx_depth;
    }
    if (frame_count < MAX_FRAMES) {
        frame_type[frame_count] = type;
        frame_len[frame_count] = plen;
        for (i = 0; i < 6; i++) {
            frame_dst[frame_count][i] = dst[i];
        }
        for (i = 0; i < plen && i < sizeof(frame_data[0]); i++) {
            frame_data[frame_count][i] = payload[i];
        }
    }
    frame_count++;
    if (tx_prints) {
        log_putc('!');
        log_putc('\n');
    }
    if (link_lost_after != 0 && frame_count == (unsigned)link_lost_after) {
        link_down = 1;
    }
    tx_depth--;
    return 0;
}

/* The network lock's balance: locks taken and not released, and whether a
   release ever came without a lock. */
static int lock_depth;
static int lock_ever_negative;

/* The tick, which the start rows move, and whether the kernel panics. */
static uint64_t host_ticks;
static int host_panicking;

/** Return the tick the rows have set. */
uint64_t tick_count(void)
{
    return host_ticks;
}

/** Report a panic once a row has set `host_panicking`. */
int k_panicking(void)
{
    return host_panicking;
}

/** Give no boot option: the carrier timeout keeps Linux's default. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    (void)key;
    if (outlen > 0) {
        out[0] = '\0';
    }
    return 0;
}

/** Take the network lock: count it and return 0 as the saved flags, since no
 * interrupt runs on the host. */
unsigned int net_lock(void)
{
    lock_depth++;
    return 0;
}

/** Release the network lock, noting a release without a lock. */
void net_unlock(unsigned int flags)
{
    (void)flags;
    lock_depth--;
    if (lock_depth < 0) {
        lock_ever_negative = 1;
    }
}

/** Give `c` to netconsole as the kernel log does with each character it
 * writes; a driver's message from inside a transmit comes back in this way. */
static void log_putc(char c)
{
    netconsole_putc(c);
}

static unsigned krandom_value = 0x1234abcdu; /* what krandom_u32 gives */

/** Return `krandom_value` as the random draw for the boot id. */
unsigned int krandom_u32(void)
{
    return krandom_value;
}

/**
 * Store `value` at `p` in network byte order.
 *
 * This, write32be and ip_checksum are the byte-order writers and the checksum
 * netconsole calls. The kernel's are in net_core.c and ip.c, which bring the
 * stack's bring-up and the whole datagram path with them, so this file has
 * its own, written independently. What they prove is the frame's layout: that
 * the fields land at the offsets the header documents, in network order, and
 * that a receiver summing the frame gets zero. That the kernel's own
 * `ip_checksum` agrees with an outside implementation is proved by `tests/run
 * netconsole`, whose checker, tests/system/checkers/pcapcheck.c, recomputes
 * the sum over a capture.
 */
void write16be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFF);
}

/** Store `value` at `p` in network byte order. */
void write32be(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value & 0xFF);
}

/** Return the one's complement of the 16-bit one's complement sum of the
 * `len` bytes at `buf`, as RFC 1071 defines the Internet checksum. */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    unsigned long sum = 0;
    uint16_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((unsigned long)buf[i] << 8) | buf[i + 1];
    }
    if (len & 1) {
        sum += (unsigned long)buf[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

/* The datagrams of netconsole= are left to the QEMU runs, which put a real
 * card and a host socket at the two ends; the target rows here drive what
 * the ring does for a target, through `eth0`, which is `the_iface`. */

/* The memory kzalloc gives, a few targets' worth, and how much is given. */
static union {
    void *align;
    unsigned char bytes[4096];
} pool;
static size_t pool_used;

/** Give `size` zeroed bytes from `pool`, or NULL when it has no more. */
void *kzalloc(size_t size)
{
    unsigned char *p = pool.bytes + pool_used;
    size_t i;

    size = (size + 15u) & ~(size_t)15u;
    if (size > sizeof(pool.bytes) - pool_used) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        p[i] = 0;
    }
    pool_used += size;
    return p;
}

/** Free nothing; `nc_reset` takes the whole pool back. */
void kfree(void *p)
{
    (void)p;
}

/** Return `the_iface` for `eth0`, and nothing for any other name. */
struct netdev *netdev_by_name(const char *name)
{
    const char *eth0 = "eth0";
    int i;

    for (i = 0; eth0[i] != '\0' && name[i] == eth0[i]; i++) {
        /* compare each character */
    }
    return (eth0[i] == '\0' && name[i] == '\0') ? &the_iface : 0;
}

/** No interface is listed here. */
struct netdev *netdev_at(int n)
{
    (void)n;
    return 0;
}

/** There is no `lo` here. */
struct netdev *netdev_loopback(void)
{
    return 0;
}

/** Refuse every IPv6 address. */
int ip6_parse(const char *s, uint32_t len, uint8_t *out)
{
    (void)s;
    (void)len;
    (void)out;
    return -1;
}

/** Format every IPv6 address as nothing. */
void ip6_format(const uint8_t *a, char *out)
{
    (void)a;
    out[0] = '\0';
}

#include "../../rach2/sys/network/netconsole.c"

/* ------------------------------------------------------------------ */
/* The test.                                                           */
/* ------------------------------------------------------------------ */

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** Put the module back to the state a fresh boot starts in. */
static void nc_reset(void)
{
    nc_head = 0;
    nc_tail = 0;
    nc_line_start = 0;
    nc_skip = 0;
    nc_settle_ticks = 0;
    nc_carrier_ticks = (uint64_t)NC_CARRIER_TIMEOUT * TICK_HZ;
    nc_tick_next = 0;
    nc_frames_start.begun = 0;
    nc_frames_start.up = 0;
    nc_frames_skipped = 0;
    nc_final = 0;
    nc_gap_first = 0;
    nc_gap_end = 0;
    nc_lost = 0;
    nc_targets = 0;
    nc_setup_done = 0;
    pool_used = 0;
    the_iface.ip = 0;
    host_ticks = 0;
    host_panicking = 0;
    nc_boot_id = 0;
    nc_seq = 0;
    nc_frames = 0;
    nc_dropped = 0;
    nc_enabled = 0;
    nc_sending = 0;
    nc_held = 0;
    frame_count = 0;
    tx_prints = 0;
    tx_depth = 0;
    tx_max_depth = 0;
    lock_depth = 0;
    lock_ever_negative = 0;
}

/** Give each character of `s` to netconsole_putc. */
static void put_string(const char *s)
{
    while (*s) {
        netconsole_putc(*s++);
    }
}

/** Return the 16-bit one's complement checksum of frame `f`, as a receiver
 * computes it; 0 means the frame is intact. */
static unsigned short frame_sum(unsigned f)
{
    return ip_checksum(frame_data[f], (unsigned short)frame_len[f]);
}

/** Read the big-endian 32-bit field at offset `off` of frame `f`. */
static unsigned frame_be32(unsigned f, unsigned off)
{
    return ((unsigned)frame_data[f][off] << 24) | ((unsigned)frame_data[f][off + 1] << 16) |
           ((unsigned)frame_data[f][off + 2] << 8) | (unsigned)frame_data[f][off + 3];
}

/** Read the big-endian 16-bit field at offset `off` of frame `f`. */
static unsigned frame_be16(unsigned f, unsigned off)
{
    return ((unsigned)frame_data[f][off] << 8) | (unsigned)frame_data[f][off + 1];
}

/**
 * Check a sender's start: a link that has just come up is sent nothing until
 * it has been up for the settle time, and the tick then sends what was held
 * with no record after it; after the start a link that comes back is sent at
 * once; a link up with nothing held ends the start at once; the carrier
 * timeout ends a start whose link never came up; and a panic, shutdown and a
 * panic in the middle of a send each send what is held.
 */
static void settle_rows(void)
{
    nc_reset();
    nc_settle_ticks = MS_TO_TICKS(NC_LINK_SETTLE_MS);
    link_down = 1;
    netconsole_enable();
    put_string("held\n");
    link_down = 0;
    host_ticks = 100;
    put_string("up\n");
    CHECK(frame_count == 0 && nc_pending() == 8, "a link just up is sent nothing");
    host_ticks = 100 + nc_settle_ticks - 1;
    put_string("a\n");
    CHECK(frame_count == 0, "nor a line a tick before the settle time");
    host_ticks = 100 + nc_settle_ticks;
    netconsole_tick();
    CHECK(frame_count == 1 && frame_len[0] == NETCONSOLE_HDR_LEN + 10 && nc_pending() == 0,
          "the tick at the settle time sends what was held, with no later record");

    link_down = 1;
    put_string("down\n");
    CHECK(frame_count == 1 && nc_pending() == 5, "after the start a link down holds the line");
    link_down = 0;
    put_string("again\n");
    CHECK(frame_count == 2 && nc_pending() == 0,
          "and a link that comes back is sent at once, with no settle");
    netconsole_tick();
    CHECK(frame_count == 2, "and the tick sends nothing once the start is over");

    nc_reset();
    nc_settle_ticks = MS_TO_TICKS(NC_LINK_SETTLE_MS);
    netconsole_enable();
    put_string("live\n");
    CHECK(frame_count == 1 && nc_pending() == 0,
          "a link up with nothing held is sent at once, with no settle");

    nc_reset();
    nc_settle_ticks = MS_TO_TICKS(NC_LINK_SETTLE_MS);
    link_down = 1;
    netconsole_enable();
    put_string("x\n");
    host_ticks = nc_carrier_ticks - 2 * NC_START_POLL_MS;
    netconsole_tick();
    CHECK(nc_frames_start.begun != 0, "the start waits for carrier until its timeout");
    host_ticks = nc_carrier_ticks;
    netconsole_tick();
    CHECK(nc_frames_start.begun == 0 && frame_count == 0,
          "and ends at the timeout, with the link still down");
    link_down = 0;
    put_string("y\n");
    CHECK(frame_count == 1 && frame_len[0] == NETCONSOLE_HDR_LEN + 4,
          "after the timeout a link that comes up is sent at once");

    nc_reset();
    nc_settle_ticks = MS_TO_TICKS(NC_LINK_SETTLE_MS);
    put_string("pre\n");
    netconsole_enable();
    put_string("settling\n");
    CHECK(frame_count == 0, "the boot log waits for the settle");
    host_panicking = 1;
    put_string("panic\n");
    CHECK(frame_count == 1 && nc_pending() == 0, "a panicking kernel sends at once");

    nc_reset();
    nc_settle_ticks = MS_TO_TICKS(NC_LINK_SETTLE_MS);
    put_string("pre\n");
    netconsole_enable();
    put_string("Goodbye!\n");
    CHECK(frame_count == 0, "the goodbye waits for the settle");
    netconsole_shutdown();
    CHECK(frame_count == 1 && frame_len[0] == NETCONSOLE_HDR_LEN + 13 && nc_pending() == 0,
          "and shutdown sends it with what was held");

    nc_reset();
    netconsole_enable();
    nc_sending = 1;
    host_panicking = 1;
    put_string("stopped\n");
    CHECK(frame_count == 0, "a record during a send only joins the ring");
    netconsole_panic();
    put_string("panic\n");
    CHECK(frame_count == 1 && nc_pending() == 0,
          "and the panic forgets the stopped send, so its record goes out");
    CHECK(lock_depth == 0 && !lock_ever_negative, "the start rows released every lock");
}

/**
 * Check that the lines wait in the ring while the card has no link: before
 * it first comes up, when the ring fills meanwhile, and when it goes down in
 * the middle of a drain, where the drain held back goes out with the next
 * newline. A card that drops every frame is offered one frame by a long line
 * and one by its newline, and gets all of it once it takes frames. The
 * start rows follow.
 */
static void link_rows(void)
{
    nc_reset();
    link_down = 1;
    netconsole_enable();
    put_string("before the link\n");
    CHECK(frame_count == 0 && nc_pending() == 16, "a card without a link is sent nothing");
    link_down = 0;
    put_string("link up\n");
    CHECK(frame_count == 1 && frame_len[0] == NETCONSOLE_HDR_LEN + 24 &&
              frame_data[0][NETCONSOLE_HDR_LEN] == 'b',
          "the next line after the link comes up sends what was held with it");

    {
        unsigned i;

        nc_reset();
        link_down = 1;
        netconsole_enable();
        for (i = 0; i < NETCONSOLE_RING + 10; i++) {
            netconsole_putc(i % 64 == 63 ? '\n' : 'f');
        }
        /* The ring fills at the newline of its 512th line, and the frames,
           held for the link, lose their oldest line to make room. */
        CHECK(frame_count == 0 && nc_pending() == NETCONSOLE_RING - 64 + 10 && nc_dropped == 0 &&
                  nc_frames_skipped == 64,
              "a ring that fills while the link is down keeps the newest, in whole lines");
        link_down = 0;
        netconsole_putc('\n');
        CHECK(frame_count > 0 && nc_pending() == 0,
              "and the next line drains it once the link is up");
    }

    {
        unsigned i;

        nc_reset();
        link_down = 1;
        netconsole_enable();
        for (i = 0; i < 3 * NETCONSOLE_PAYLOAD_MAX; i++) {
            netconsole_putc('m');
        }
        link_down = 0;
        link_lost_after = 1;
        netconsole_putc('\n');
        CHECK(frame_count == 1 && nc_pending() == 2 * NETCONSOLE_PAYLOAD_MAX + 1,
              "a frame the lost link refuses stays in the ring with the rest");
        link_down = 0;
        link_lost_after = 0;
        put_string("back\n");
        CHECK(frame_count == 4 && nc_pending() == 0 && frame_be32(3, 8) == 3 &&
                  frame_data[1][NETCONSOLE_HDR_LEN] == 'm' &&
                  frame_data[3][NETCONSOLE_HDR_LEN] == '\n',
              "and goes out, in order, with the next line once the link is back");
    }

    {
        unsigned i;

        nc_reset();
        netconsole_enable();
        refuse_all = 1;
        tx_attempts = 0;
        for (i = 0; i < 3 * NETCONSOLE_PAYLOAD_MAX; i++) {
            netconsole_putc('r');
        }
        CHECK(tx_attempts == 1, "a card refusing every frame is offered one by a long line");
        netconsole_putc('\n');
        CHECK(tx_attempts == 2 && frame_count == 0, "and one more by its newline");
        refuse_all = 0;
        netconsole_putc('\n');
        CHECK(nc_pending() == 0 && frame_count == 4, "and all of it once the card takes frames");
    }

    settle_rows();
}

/** Whether the payload of datagram `f`, after its IPv4 and UDP headers, is
 * `want` and no more. */
static int datagram_is(unsigned f, const char *want)
{
    unsigned i;

    for (i = 0; want[i] != '\0'; i++) {
        if (NC_IP4_HDR + NC_UDP_HDR + i >= frame_len[f] ||
            frame_data[f][NC_IP4_HDR + NC_UDP_HDR + i] != (unsigned char)want[i]) {
            return 0;
        }
    }
    return frame_len[f] == NC_IP4_HDR + NC_UDP_HDR + i;
}

/** Whether the payload of datagram `f` begins with `want`. */
static int datagram_begins(unsigned f, const char *want)
{
    unsigned i;

    for (i = 0; want[i] != '\0'; i++) {
        if (NC_IP4_HDR + NC_UDP_HDR + i >= frame_len[f] ||
            frame_data[f][NC_IP4_HDR + NC_UDP_HDR + i] != (unsigned char)want[i]) {
            return 0;
        }
    }
    return 1;
}

/** Write `lines` lines of 63 `f`s and a newline. */
static void put_lines(unsigned lines)
{
    unsigned i;

    for (i = 0; i < 64 * lines; i++) {
        netconsole_putc(i % 64 == 63 ? '\n' : 'f');
    }
}

/**
 * Check a target of netconsole= beside the frames: a target with no source
 * address loses its own oldest records when the ring fills, and the frames
 * none, and is then told of them by Linux's dropped line in the plain form
 * and by a gap in `seq` in the extended form; and records the full ring
 * dropped before setup reach a target as the dropped line at their place.
 */
static void target_rows(void)
{
    static char plain[] = "@/,@10.0.0.1/";
    static char extended[] = "+@/,@10.0.0.1/";
    static char alone[] = "@/,@10.0.0.1/";

    nc_reset();
    CHECK(netconsole_setup(plain, 1) == 1 && nc_targets && nc_targets->held,
          "a target with no source address is held");
    put_lines(600);
    /* The target's place is the ring's whole 32767 at the 32768th
       character and at each 64 after it: 89 lines. */
    CHECK(nc_frames == 600 && nc_dropped == 0 && nc_frames_skipped == 0,
          "the frames are sent every line while the target is held");
    CHECK(nc_targets->lost == 89 && nc_targets->skipped == 89 * 64,
          "the held target loses its own oldest lines");
    the_iface.ip = 0x0A000002u;
    frame_count = 0;
    put_string("next\n");
    CHECK(frame_count > 3 && frame_type[0] == NETCONSOLE_ETHERTYPE && frame_type[1] == ETH_TYPE_IP,
          "the frames' line goes first, then the target's datagrams");
    CHECK(datagram_is(1, "** 89 printk messages dropped **\n"),
          "the target is told what it lost, as Linux's line");
    CHECK(frame_len[2] == NC_IP4_HDR + NC_UDP_HDR + 64 && nc_targets->lost == 0 &&
              nc_targets->datagrams == 1 + 511 + 1,
          "and is then sent every line it kept, and the new one");

    nc_reset();
    (void)netconsole_setup(extended, 1);
    put_lines(600);
    the_iface.ip = 0x0A000002u;
    frame_count = 0;
    put_string("next\n");
    CHECK(datagram_begins(1, "0,89,0,c;fff") && datagram_begins(2, "0,90,0,c;fff"),
          "an extended target's seq goes past the 89 records it lost");

    nc_reset();
    put_lines(513);
    put_string("tail");
    /* The ring is full at the 512th line's newline: that line and the next
       are dropped whole, newest first, since no sender exists yet. */
    CHECK(nc_dropped == 128 && nc_gap_end == 2, "a full ring with no sender drops the newest");
    the_iface.ip = 0x0A000002u;
    (void)netconsole_setup(alone, 0);
    CHECK(nc_targets && nc_targets->lost == 2 && nc_targets->datagrams == 511,
          "a new target is sent the lines before the gap, and counts it");
    frame_count = 0;
    put_string("\n");
    CHECK(frame_count == 2 && datagram_is(0, "** 2 printk messages dropped **\n") &&
              datagram_is(1, "tail\n"),
          "and is told of it before the next line");
    CHECK(lock_depth == 0 && !lock_ever_negative, "the target rows released every lock");
}

/** Check that a zero random draw does not become a zero boot id. */
static void boot_id_row(void)
{
    nc_reset();
    krandom_value = 0;
    netconsole_enable();
    put_string("x\n");
    CHECK(frame_count == 1 && frame_be32(0, 4) != 0, "a zero draw does not become a zero boot id");
    krandom_value = 0x1234abcdu;
}

/**
 * Run the rows: lines printed before netconsole_enable are held and sent in
 * one frame whose header, boot id, length and checksum are checked; the link
 * rows, with the start's; the target rows; a zero random draw does not
 * become a zero boot id; each line is a frame numbered in order; an
 * unterminated line waits; a full payload goes without a newline; the ring
 * is bounded and counts what it drops; a full ring drains in order; a driver
 * printing from inside a transmit does not recurse, and its message waits
 * for the next flush; and a console never enabled sends nothing.
 *
 * Returns the number of failed checks, at most 255.
 */
int main(void)
{
    nc_reset();
    put_string("boot line one\n");
    put_string("boot line two\n");
    CHECK(frame_count == 0, "nothing is transmitted before the console is enabled");
    CHECK(nc_pending() == 28, "the characters are held instead");

    netconsole_enable();
    CHECK(frame_count == 1, "enabling drains the buffer in one frame");
    CHECK(frame_type[0] == NETCONSOLE_ETHERTYPE, "the frame has the netconsole ethertype");
    CHECK(frame_dst[0][0] == 0xFF && frame_dst[0][5] == 0xFF, "the frame is broadcast");
    CHECK(frame_len[0] == NETCONSOLE_HDR_LEN + 28, "the frame is header plus the held bytes");
    CHECK(frame_data[0][0] == 'R' && frame_data[0][1] == 'C' && frame_data[0][2] == 'H' &&
              frame_data[0][3] == '2',
          "the frame has the magic");
    CHECK(frame_be32(0, 4) == krandom_value, "the boot id is the drawn value");
    CHECK(frame_be32(0, 8) == 0, "the first frame is sequence zero");
    CHECK(frame_be16(0, 12) == 28, "the length field is the payload length");
    CHECK(frame_sum(0) == 0, "the frame sums to zero, as its receiver checks");
    CHECK(frame_data[0][NETCONSOLE_HDR_LEN] == 'b', "the payload starts at the first held byte");
    CHECK(nc_pending() == 0, "the buffer is empty afterwards");
    CHECK(lock_depth == 0 && !lock_ever_negative, "every lock taken was released");

    link_rows();
    target_rows();
    boot_id_row();

    nc_reset();
    netconsole_enable();
    put_string("one\n");
    put_string("two\n");
    put_string("three\n");
    CHECK(frame_count == 3, "each completed line is a frame");
    CHECK(frame_be32(0, 8) == 0 && frame_be32(1, 8) == 1 && frame_be32(2, 8) == 2,
          "the sequence numbers count up");
    CHECK(frame_be32(0, 4) == frame_be32(2, 4), "every frame has the same boot id");
    CHECK(frame_sum(0) == 0 && frame_sum(1) == 0 && frame_sum(2) == 0, "every frame sums to zero");

    nc_reset();
    netconsole_enable();
    put_string("no newline yet");
    CHECK(frame_count == 0, "an unterminated line is not sent");
    netconsole_putc('\n');
    CHECK(frame_count == 1 && frame_len[0] == NETCONSOLE_HDR_LEN + 15,
          "the newline sends the whole line");

    {
        unsigned i;

        nc_reset();
        netconsole_enable();
        for (i = 0; i < NETCONSOLE_PAYLOAD_MAX; i++) {
            netconsole_putc('a');
        }
        CHECK(frame_count == 1, "a payload's worth is sent without waiting for a newline");
        CHECK(frame_len[0] == NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX,
              "the frame has a full payload and no more");
        CHECK(frame_sum(0) == 0, "the full frame sums to zero");
    }

    {
        unsigned i;

        nc_reset();
        for (i = 0; i < NETCONSOLE_RING + 100; i++) {
            netconsole_putc('z');
        }
        CHECK(nc_dropped == 101, "the overflow is counted, not written past the ring");
        CHECK(nc_pending() == NETCONSOLE_RING - 1, "the ring has its capacity and no more");
        CHECK(nc_head < NETCONSOLE_RING && nc_tail < NETCONSOLE_RING,
              "both indices stay inside the ring");
    }

    {
        unsigned i, seq;

        nc_reset();
        for (i = 0; i < NETCONSOLE_RING - 1; i++) {
            netconsole_putc('q');
        }
        netconsole_enable();
        CHECK(frame_count ==
                  (NETCONSOLE_RING - 1 + NETCONSOLE_PAYLOAD_MAX - 1) / NETCONSOLE_PAYLOAD_MAX,
              "a full ring drains in ceil(ring / payload) frames");
        CHECK(nc_pending() == 0, "the ring is empty after the drain");
        for (seq = 0; seq < frame_count && seq < MAX_FRAMES; seq++) {
            if (frame_be32(seq, 8) != seq || frame_sum(seq) != 0) {
                fails++;
                printf("  FAIL: frame %u of the drain is out of order or corrupt\n", seq);
                break;
            }
        }
        CHECK(lock_depth == 0 && !lock_ever_negative, "the drain released every lock");
    }

    nc_reset();
    netconsole_enable();
    tx_prints = 1;
    put_string("trigger\n");
    CHECK(tx_max_depth == 1, "the transmit is never entered from inside itself");
    CHECK(frame_count == 1, "the driver's own message did not become a second frame");
    CHECK(nc_pending() == 2, "the driver's own message is held in the ring instead");
    tx_prints = 0;
    netconsole_flush();
    CHECK(frame_count == 2 && frame_be32(1, 8) == 1,
          "the held message goes out on the next flush, in sequence");

    {
        unsigned i;

        nc_reset();
        for (i = 0; i < 100; i++) {
            put_string("still down\n");
        }
        CHECK(frame_count == 0, "a console that was never enabled sends nothing");
        CHECK(netconsole_is_enabled() == 0, "and reports itself disabled");
        netconsole_flush();
        CHECK(frame_count == 0, "an explicit flush on a disabled console sends nothing");
    }

    return fails > 255 ? 255 : fails;
}
