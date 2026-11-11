/*
 * netconsole.c: the kernel log, repeated onto the wire as UDP datagrams to
 * the targets of `netconsole=` and as broadcast frames, as netconsole.h
 * describes it.
 *
 * The output path, `netconsole_putc` and the flush it starts, runs inside
 * `klog`'s delivery, which is entered from anywhere in the kernel, interrupt
 * handlers and the panic path included, so it does three things and no
 * others: it appends a character to a fixed ring, it fills a fixed frame
 * buffer, and it calls the Ethernet layer. It allocates nothing and waits for
 * nothing. Only `netconsole_setup`, at boot, allocates the targets.
 *
 * The ring keeps what the log writes before a card is up: the memory layout,
 * the module ranges, the device probes, which the first frames carry. Each
 * sender starts as Linux's `netpoll_setup` starts one, by waiting for its
 * card's carrier, at most `netpoll.carrier_timeout` seconds; when the link
 * comes up in that wait with records held, they wait a further settle time
 * (`nc_link_ready`). The timer's tick drives the start (`netconsole_tick`),
 * so the held records go out when it ends and not with a later record, and
 * `kernel_shutdown` sends what is left before it stops the cards. After the
 * start a record goes out when it is written. A frame the card refuses,
 * because the link is down or its transmit ring is full, stays in the ring,
 * and the drain stops until the next line: characters leave the ring only
 * once their frame is sent, which also keeps a character that arrives during
 * the send from taking their place. While a drain is held so, only a newline
 * starts another, and not the 512 waiting characters that otherwise do
 * (`nc_held`), so the card is asked once a line.
 *
 * Two things can interleave here, and each has its own guard.
 *
 * Interrupts. A system call logs with interrupts on, and the timer and the
 * cards log from their handlers, so the ring and the Ethernet layer's frame
 * buffer can each be entered from two contexts. net.h gives the rule for the
 * frame buffer, and the ring needs the same: without it, a handler that logs
 * between a writer computing its next index and storing it writes into the
 * slot the writer then overwrites, and a character is lost from the log with
 * nothing to show for it. So `net_lock` brackets the ring's update and each
 * frame; each frame and not the whole drain, since a full ring is sixteen
 * frames and the timer's interrupt should not wait for all of them.
 *
 * Recursion. A driver reports a transmit error with a record of its own,
 * which comes back here in the same context, where disabling interrupts
 * protects nothing. A record raised during `klog`'s delivery waits in `klog`'s
 * queue until that delivery is done, so it cannot. One raised inside the
 * flush `netconsole_enable` starts can, since that flush is no delivery.
 * `nc_sending` stops it: while a frame is being sent, a character only joins
 * the ring. The characters it adds would still extend the drain that is
 * running, so a drain fixes its end at the ring's head when it starts. A card
 * that refuses every frame costs one refused frame a line. A panic clears
 * `nc_sending` (`netconsole_panic`), since the send it marks never resumes.
 *
 * The targets. Each keeps its own place in the ring, and the ring frees a
 * character once the frames and every target have sent it: `nc_tail` is the
 * frames' place, or, without them, the place of the target furthest behind.
 * A sender that cannot send, for want of a link or a source address, holds
 * back only its own records: when the ring is full, a held sender furthest
 * behind loses its oldest record and the newest is kept (`nc_make_room`), as
 * a console behind Linux's log buffer loses the oldest. The ring drops the
 * newest only before any sender exists, which keeps the boot log, or while a
 * drain runs. A target is told what it lost as Linux tells a console: a gap
 * in the extended form's `seq`, or `** N printk messages dropped **` before
 * the next plain record (kernel/printk/printk.c:2904-2906, 3049-3060). A
 * target is sent whole lines, which are whole records,
 * since a record is one line: one datagram a record, as Linux's `write_msg`
 * sends one a message (drivers/net/netconsole.c). The datagram is built as
 * Linux's `netpoll_send_udp` builds it (net/core/netpoll.c): IPv4 with TTL 64
 * and an identification that counts, or IPv6 with hop limit 32, and a UDP
 * checksum over the pseudo-header, 0xffff in place of 0.
 */

#include "../klog.h"
#include "../panic.h"
#include "../timer.h"
#include "../cmdline.h"
#include "netconsole.h"
#include "net.h"
#include "core/byteorder.h"
#include "ethernet/ethernet.h"
#include "driver/netdev.h"
#include "ip/ip.h"
#include "../lib/krandom.h"
#include "../mm/kheap.h"
#include "../syscall.h"
#include "ip6/ip6.h"

/* The characters not yet sent, a boot log's worth ahead of the probe: the
 * Lenovo writes some 17000 before its link is up. One place is kept empty to
 * tell a full ring from an empty one. */
#define NETCONSOLE_RING 32768

static char nc_ring[NETCONSOLE_RING];
static uint32_t nc_head;       /* where the next character goes */
static uint32_t nc_tail;       /* the oldest character not yet sent */
static uint32_t nc_line_start; /* where the line being written began */

/* What a full ring does with the rest of the line being written: 0, nothing,
 * since none was dropped; NC_SKIP_LINE, drop it, its start having been taken
 * back out of the ring; NC_SKIP_END, drop it but end its start, which a
 * sender already has, with the first newline that finds room. */
#define NC_SKIP_LINE 1
#define NC_SKIP_END  2
static int nc_skip;

/* The frame payload being built: the header, then the characters. */
static uint8_t nc_payload[NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX];

static uint32_t nc_boot_id;
static uint32_t nc_seq;     /* the next frame's sequence number */
static uint32_t nc_frames;  /* frames the card took */
static uint32_t nc_dropped; /* characters the ring had no room for */
static int nc_enabled;
static int nc_sending; /* a drain runs: a character only joins the ring */
static int nc_held;    /* the last drain stopped: no link or a refused frame */

static const uint8_t nc_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* How long a link that came up during a sender's start must have been up
 * before the records held for it are sent. The Lenovo's RTL8168 counted
 * every frame of the boot log it sent the moment its link came up as sent,
 * in its own tally (TxOK) as in the driver's count, yet none reached the
 * Mac, while every record sent from 0.85 s later on did, and the same drain
 * sent 2 s after the link came up arrived whole: what is sent at once is
 * lost past the card, where no driver can see it. Linux 6.12 waits for
 * carrier and no longer (`netpoll_setup`, net/core/netpoll.c:694-716), and
 * would lose it too; the settle is Rach2's own. */
#define NC_LINK_SETTLE_MS 2000
static uint32_t nc_settle_ticks = MS_TO_TICKS(NC_LINK_SETTLE_MS);

/* The longest a sender's start waits for carrier, in seconds: Linux's
 * `carrier_timeout`, the boot option `netpoll.carrier_timeout=`
 * (net/core/netpoll.c:61-62). */
#define NC_CARRIER_TIMEOUT 4
static uint64_t nc_carrier_ticks = (uint64_t)NC_CARRIER_TIMEOUT * TICK_HZ;

/* How often the tick tries a drain while a sender starts. */
#define NC_START_POLL_MS 10
static uint64_t nc_tick_next; /* the tick of the next try */

/**
 * A sender's start, as `nc_link_ready` follows it: the wait for carrier, then
 * the settle. Each tick is kept plus one, so that 0 means none.
 */
struct nc_start {
    uint64_t begun; /* the tick the sender began; 0 once its start is over */
    uint64_t up;    /* the tick its link was last seen come up in the start */
};

static struct nc_start nc_frames_start;
static uint32_t nc_frames_skipped; /* characters the frames lost to `nc_make_room` */
static int nc_final;               /* `netconsole_shutdown` ran: no sender waits any more */

/* Records the full ring dropped for every sender, each run kept with the place
 * of the record after it, so that each target counts them when it reaches
 * that place: a ring of NC_GAPS runs, from the id `nc_gap_first` up to
 * `nc_gap_end`. A run with no room left is added to the newest, which a
 * target then counts a few records early. `nc_lost` counts the records
 * dropped since the last character stored, whose place is not known until
 * the next record starts. */
#define NC_GAPS 16
static struct {
    uint32_t at;   /* the ring place of the record after the run */
    uint32_t lost; /* the records dropped */
} nc_gaps[NC_GAPS];
static uint32_t nc_gap_first;
static uint32_t nc_gap_end;
static uint32_t nc_lost;

/* The longest piece of a line one extended datagram carries before escaping:
 * a whole record, KLOG_RECORD_MAX. */
#define NC_EXT_LINE KLOG_RECORD_MAX

/* An extended message whole: the release and its comma, the header, and the
 * line with each byte escaped to at most four. */
#define NC_EXT_MAX (sizeof(UTS_RELEASE) + 64 + 4 * NC_EXT_LINE + 1)

/* The IPv4 and IPv6 headers, and the UDP header after either. */
#define NC_IP4_HDR 20
#define NC_IP6_HDR 40
#define NC_UDP_HDR 8

/**
 * One target of `netconsole=`, as Linux's `struct netpoll` and the two flags
 * of `struct netconsole_target` hold one. Addresses are in network order, an
 * IPv4 address in the first four bytes.
 */
struct nc_target {
    struct nc_target *next;
    struct netdev *nd; /* the interface `dev` named, found at setup */
    int ipv6;
    int extended; /* `+`: Linux's extended form */
    int release;  /* `r`: the release before it */
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t local_ip[16]; /* all zero: the interface's, at each datagram */
    uint8_t remote_ip[16];
    uint8_t remote_mac[6];
    uint32_t tail; /* the oldest character not yet sent to it */
    int held;      /* its last drain stopped: no link, no source, refused */
    struct nc_start start;
    uint32_t datagrams; /* datagrams the card took */
    uint32_t seq;       /* the next extended record's sequence number */
    uint32_t gap;       /* the id of the next run in `nc_gaps` it counts */
    uint32_t lost;      /* records lost and not yet shown in `seq` or a line */
    uint32_t skipped;   /* characters lost to `nc_make_room` */
    int level;          /* the last record's level, for a piece of a longer line */
    char usec[24];      /* and its stamp in microseconds, as digits */
};

static struct nc_target *nc_targets;
static int nc_setup_done;
static uint16_t nc_ip_id; /* the last IPv4 identification sent */

/* The line being sent, its extended form, and the datagram, IPv6 header the
 * largest, for which ethernet_tx adds the Ethernet header. */
static char nc_line[NETCONSOLE_CHUNK_MAX];
static char nc_ext[NC_EXT_MAX];
static uint8_t nc_pkt[NC_IP6_HDR + NC_UDP_HDR + NETCONSOLE_CHUNK_MAX];

/** How many characters wait behind the place `tail`. */
static uint32_t nc_behind(uint32_t tail)
{
    return (nc_head >= tail) ? (nc_head - tail) : (NETCONSOLE_RING - tail + nc_head);
}

/** How many characters are waiting for the frames, or, without them, for the
 * target furthest behind. */
static uint32_t nc_pending(void)
{
    return nc_behind(nc_tail);
}

/** How many characters the ring holds: what the sender furthest behind has
 * still to send. */
static uint32_t nc_used(void)
{
    uint32_t most = nc_pending();
    struct nc_target *t;

    for (t = nc_targets; t; t = t->next) {
        if (nc_behind(t->tail) > most) {
            most = nc_behind(t->tail);
        }
    }
    return most;
}

/** Whether no sender has taken any of the line being written: each one's
 * place is at its start or before it. */
static int nc_line_unsent(void)
{
    uint32_t written = nc_behind(nc_line_start);
    struct nc_target *t;

    if (nc_behind(nc_tail) < written) {
        return 0;
    }
    for (t = nc_targets; t; t = t->next) {
        if (nc_behind(t->tail) < written) {
            return 0;
        }
    }
    return 1;
}

/** Without the frames, move `nc_tail` to the place of the target furthest
 * behind, which then frees the ring. Under `net_lock`. */
static void nc_follow(void)
{
    uint32_t most = 0;
    struct nc_target *t;

    if (nc_enabled || !nc_targets) {
        return;
    }
    for (t = nc_targets; t; t = t->next) {
        if (nc_behind(t->tail) >= most) {
            most = nc_behind(t->tail);
            nc_tail = t->tail;
        }
    }
}

/** Forget the runs of `nc_gaps` that every target has counted, and all of
 * them once setup has given no target. Under `net_lock`. */
static void nc_retire_gaps(void)
{
    uint32_t most = 0;
    struct nc_target *t;

    for (t = nc_targets; t; t = t->next) {
        if (nc_gap_end - t->gap > most) {
            most = nc_gap_end - t->gap;
        }
    }
    if (nc_targets || nc_setup_done) {
        nc_gap_first = nc_gap_end - most;
    }
}

/** Keep the `nc_lost` records just dropped as a run before the place `at`,
 * where the next record starts, as the comment on `nc_gaps` says. Under
 * `net_lock`. */
static void nc_note_gap(uint32_t at)
{
    struct nc_target *t;

    nc_retire_gaps();
    if (nc_gap_end - nc_gap_first < NC_GAPS) {
        nc_gaps[nc_gap_end % NC_GAPS].at = at;
        nc_gaps[nc_gap_end % NC_GAPS].lost = nc_lost;
        nc_gap_end++;
    } else {
        nc_gaps[(nc_gap_end - 1) % NC_GAPS].lost += nc_lost;
        for (t = nc_targets; t; t = t->next) {
            if (t->gap == nc_gap_end) {
                t->lost += nc_lost;
            }
        }
    }
    nc_lost = 0;
}

/** Count against `t` each run of dropped records at its place, which is
 * where the record after the run starts. Under `net_lock`. */
static void nc_pass_gaps(struct nc_target *t)
{
    while (t->gap != nc_gap_end && nc_gaps[t->gap % NC_GAPS].at == t->tail) {
        t->lost += nc_gaps[t->gap % NC_GAPS].lost;
        t->gap++;
    }
}

/** Move `*place` past the next newline before `nc_head`, and return the
 * characters it passed; 0, leaving it, when there is none. */
static uint32_t nc_skip_record(uint32_t *place)
{
    uint32_t at = *place, n = 0;

    while (at != nc_head) {
        char c = nc_ring[at];

        n++;
        at = (at + 1 == NETCONSOLE_RING) ? 0 : at + 1;
        if (c == '\n') {
            *place = at;
            return n;
        }
    }
    return 0;
}

/**
 * Make room in the full ring by moving each sender furthest behind past its
 * oldest record, which that sender loses, when each such sender is held: one
 * that cannot send then costs the others nothing, as one console that falls
 * behind Linux's log buffer loses its own oldest records and no other's
 * (`console_emit_next_record`, kernel/printk/printk.c:3049). Does nothing,
 * leaving the newest to be dropped for all, before any sender exists, which
 * keeps the boot log, while a drain runs, whose sender's place must not move,
 * or when a sender furthest behind is not held. Under `net_lock`.
 */
static void nc_make_room(void)
{
    struct nc_target *t;
    uint32_t most, n;
    int frames;

    if (nc_sending || (!nc_enabled && !nc_targets)) {
        return;
    }
    nc_follow();
    most = nc_used();
    frames = nc_enabled && nc_pending() == most;
    if (frames && !nc_held) {
        return;
    }
    for (t = nc_targets; t; t = t->next) {
        if (nc_behind(t->tail) == most && !t->held) {
            return;
        }
    }
    if (frames) {
        nc_frames_skipped += nc_skip_record(&nc_tail);
    }
    for (t = nc_targets; t; t = t->next) {
        if (nc_behind(t->tail) == most) {
            nc_pass_gaps(t);
            n = nc_skip_record(&t->tail);
            if (n != 0) {
                t->skipped += n;
                t->lost++;
            }
        }
    }
    nc_follow();
}

/**
 * Decide whether the ring drops `c`, as `netconsole_putc` says, and count it
 * when it does. A full ring first takes the oldest record from held senders
 * (`nc_make_room`); when that frees nothing, it drops the newest records
 * whole, so that a sender is given only whole ones, as Linux's consoles are
 * given whole records and lose whole ones (kernel/printk/printk.c:2967). The
 * start of the record it fills in on is taken back out when no sender has
 * any of it yet and it is no longer than a datagram; when one has, the record
 * is cut and ended with the next newline that finds room, so a cut record is
 * never joined to a later one. Each newline dropped is a record lost
 * (`nc_lost`): a cut record's own newline stands for the record whose newline
 * ends it. Under `net_lock`.
 */
static int nc_drop(char c)
{
    if (nc_used() == NETCONSOLE_RING - 1 &&
        (nc_skip == 0 || (nc_skip == NC_SKIP_END && c == '\n'))) {
        nc_make_room();
    }
    if (nc_skip == 0 && nc_used() == NETCONSOLE_RING - 1) {
        if (!nc_sending && nc_behind(nc_line_start) <= NETCONSOLE_CHUNK_MAX && nc_line_unsent()) {
            nc_dropped += nc_behind(nc_line_start);
            nc_head = nc_line_start;
            nc_skip = NC_SKIP_LINE;
        } else {
            nc_skip = NC_SKIP_END;
        }
    }
    if (nc_skip == NC_SKIP_LINE ||
        (nc_skip == NC_SKIP_END && (c != '\n' || nc_used() == NETCONSOLE_RING - 1))) {
        nc_dropped++;
        if (c == '\n') {
            nc_lost++;
            if (nc_skip == NC_SKIP_LINE) {
                nc_skip = 0;
            }
        }
        return 1;
    }
    return 0;
}

/**
 * Take one character, as netconsole.h says: into the ring unless `nc_drop`
 * drops it, after a run of dropped records noted at the start of the next
 * (`nc_note_gap`). A newline still starts a drain, which makes room.
 */
void netconsole_putc(char c)
{
    uint32_t flags = net_lock();
    int stored = 0;
    int send;

    if (!nc_drop(c)) {
        if (nc_lost != 0 && nc_head == nc_line_start) {
            nc_note_gap(nc_head);
        }
        nc_ring[nc_head] = c;
        nc_head = (nc_head + 1 == NETCONSOLE_RING) ? 0 : nc_head + 1;
        if (c == '\n') {
            nc_line_start = nc_head;
            nc_skip = 0;
        }
        stored = 1;
    }

    send =
        (nc_enabled || nc_targets) && !nc_sending &&
        (c == '\n' || (stored && nc_enabled && !nc_held && nc_pending() >= NETCONSOLE_PAYLOAD_MAX));
    net_unlock(flags);

    if (send) {
        netconsole_flush();
    }
}

/** Begin the start of a sender, as `nc_link_ready` follows it. */
static void nc_begin(struct nc_start *s)
{
    s->begun = tick_count() + 1;
    s->up = 0;
}

/**
 * Whether a drain may send on `nd`, for a sender with `waiting` characters
 * held and its start `*s`.
 *
 * After the start, whenever the link is up (`netdev_running`); ethernet.c
 * sends nothing on a link that is down. Linux's `write_msg` asks only
 * `netif_running` (drivers/net/netconsole.c:1214) and `__netpoll_send_skb`
 * no more (net/core/netpoll.c:332), never the carrier.
 *
 * In the start, not while the link is down, as `netpoll_setup` waits for
 * carrier (net/core/netpoll.c:706-714), until `nc_carrier_ticks` from its
 * beginning, when the start ends as Linux's "timeout waiting for carrier"
 * does. The link seen up ends it at once when nothing is held; otherwise the
 * held records wait until the link has been up for `nc_settle_ticks`, and a
 * link that goes down meanwhile starts that time again. A panic and
 * `netconsole_shutdown` end the start, since there is no later. Under
 * `net_lock`.
 */
static int nc_link_ready(struct netdev *nd, struct nc_start *s, uint32_t waiting)
{
    uint64_t now;

    if (s->begun != 0 && (k_panicking() || nc_final)) {
        s->begun = 0;
    }
    if (s->begun == 0) {
        return netdev_running(nd);
    }
    now = tick_count() + 1;
    if (!netdev_running(nd)) {
        s->up = 0;
        if (now - s->begun >= nc_carrier_ticks) {
            s->begun = 0;
        }
        return 0;
    }
    if (s->up == 0) {
        s->up = now;
    }
    if (waiting == 0 || now - s->up >= nc_settle_ticks) {
        s->begun = 0;
        return 1;
    }
    return 0;
}

/** Send the ring to the first interface in broadcast frames, as
 * `netconsole_flush` says. */
static void nc_flush_frames(void)
{
    uint32_t flags = net_lock();
    uint32_t end;

    if (!nc_enabled || nc_sending) {
        net_unlock(flags);
        return;
    }
    if (!nc_link_ready(netdev_first(), &nc_frames_start, nc_pending())) {
        nc_held = 1;
        net_unlock(flags);
        return;
    }
    nc_sending = 1;
    end = nc_head;
    net_unlock(flags);

    for (;;) {
        uint16_t n = 0;
        uint32_t at;

        flags = net_lock();
        at = nc_tail;
        while (at != end && n < NETCONSOLE_PAYLOAD_MAX) {
            nc_payload[NETCONSOLE_HDR_LEN + n] = (uint8_t)nc_ring[at];
            n++;
            at++;
            if (at == NETCONSOLE_RING) {
                at = 0;
            }
        }
        if (n == 0) {
            nc_sending = 0;
            net_unlock(flags);
            return;
        }

        nc_payload[0] = 'R';
        nc_payload[1] = 'C';
        nc_payload[2] = 'H';
        nc_payload[3] = '2';
        write32be(nc_payload + 4, nc_boot_id);
        write32be(nc_payload + 8, nc_seq);
        write16be(nc_payload + 12, n);
        write16be(nc_payload + 14, 0);
        write16be(nc_payload + 14, ip_checksum(nc_payload, (uint16_t)(NETCONSOLE_HDR_LEN + n)));

        if (ethernet_tx(netdev_first(), nc_broadcast, NETCONSOLE_ETHERTYPE, nc_payload,
                        (uint16_t)(NETCONSOLE_HDR_LEN + n)) != NETDEV_TX_OK) {
            nc_held = 1;
            nc_sending = 0;
            net_unlock(flags);
            return;
        }
        nc_held = 0;
        nc_tail = at;
        nc_seq++;
        nc_frames++;
        net_unlock(flags);
    }
}

/* ---- the targets ---- */

/** Add the `len` bytes at `p` to the one's complement sum `sum`, as 16-bit
 * big-endian words, the last byte of an odd length padded with zero. */
static uint32_t nc_sum(uint32_t sum, const uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    if (len & 1u) {
        sum += (uint32_t)p[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return sum;
}

/** Whether the `n` bytes at `p` are all zero. */
static int nc_zero(const uint8_t *p, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/** Whether `a` is an IPv6 link-local address, fe80::/10. */
static int nc_linklocal(const uint8_t *a)
{
    return a[0] == 0xFE && (a[1] & 0xC0) == 0x80;
}

/**
 * Write the address `t` sends from to `src`: the one it gave, or the
 * interface's first IPv4 address, or for IPv6 its first address whose
 * link-local scope matches the target's, as Linux's `netpoll_setup` chooses
 * (net/core/netpoll.c:719-756). Rach2 also passes over an address that is
 * tentative or failed duplicate detection, which Linux does not, since
 * nothing on the link would answer it yet. Returns 0 while there is none.
 * Under `net_lock`.
 */
static int nc_source(const struct nc_target *t, uint8_t *src)
{
    int i;

    if (!nc_zero(t->local_ip, t->ipv6 ? 16 : 4)) {
        for (i = 0; i < 16; i++) {
            src[i] = t->local_ip[i];
        }
        return 1;
    }
    if (!t->ipv6) {
        if (t->nd->ip == 0) {
            return 0;
        }
        write32be(src, t->nd->ip);
        return 1;
    }
    for (i = 0; i < t->nd->naddr6; i++) {
        const struct netdev_addr6 *a = &t->nd->addr6[i];
        int j;

        if ((a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)) != 0 ||
            nc_linklocal(a->addr) != nc_linklocal(t->remote_ip)) {
            continue;
        }
        for (j = 0; j < 16; j++) {
            src[j] = a->addr[j];
        }
        return 1;
    }
    return 0;
}

/**
 * Copy into `nc_line` the line that starts at the place `from`, its newline
 * included, and return its length: at most `limit` characters, a longer line
 * going in pieces of `limit`. Returns 0 when the line before `end` is not
 * whole and shorter than `limit`, since its newline is still to come.
 */
static uint32_t nc_take(uint32_t from, uint32_t end, uint32_t limit)
{
    uint32_t n = 0;

    while (from != end && n < limit) {
        char c = nc_ring[from];

        nc_line[n++] = c;
        from++;
        if (from == NETCONSOLE_RING) {
            from = 0;
        }
        if (c == '\n') {
            return n;
        }
    }
    return n == limit ? n : 0;
}

/** Write `v` in decimal at `out` and return the digits written. */
static uint32_t nc_put_u32(char *out, uint32_t v)
{
    char digits[10];
    uint32_t n = 0, i;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    for (i = 0; i < n; i++) {
        out[i] = digits[n - 1 - i];
    }
    return n;
}

/**
 * Read the record prefix `<level>[seconds.microseconds] ` that begins the
 * `n` characters of `nc_line` into `t->level` and `t->usec`, the stamp as one
 * number of microseconds with no leading zeros, and return where the text
 * after it starts; 0, leaving both as they were, when the line has none.
 */
static uint32_t nc_prefix(struct nc_target *t, uint32_t n)
{
    char usec[sizeof(t->usec)];
    uint32_t k = 1, u = 0, j;
    int level = 0;

    if (n == 0 || nc_line[0] != '<') {
        return 0;
    }
    while (k < 4 && k < n && nc_line[k] >= '0' && nc_line[k] <= '9') {
        level = level * 10 + (nc_line[k++] - '0');
    }
    if (k == 1 || k + 1 >= n || nc_line[k] != '>' || nc_line[k + 1] != '[') {
        return 0;
    }
    for (k += 2; k < n && u + 1 < sizeof(usec); k++) {
        char c = nc_line[k];

        if (c != '.' && (c < '0' || c > '9')) {
            break;
        }
        if (c != '.' && (u > 0 || c != '0')) {
            usec[u++] = c;
        }
    }
    if (k + 1 >= n || nc_line[k] != ']' || nc_line[k + 1] != ' ') {
        return 0;
    }
    if (u == 0) {
        usec[u++] = '0';
    }
    usec[u] = '\0';
    for (j = 0; j <= u; j++) {
        t->usec[j] = usec[j];
    }
    t->level = level;
    return k + 2;
}

/**
 * Write the `n` characters of `nc_line` in Linux's extended form to `nc_ext`
 * and return its length: the release and a comma for `r`, then
 * `level,seq,usec,flags;` as `info_print_ext_header` writes it, then the
 * text with every byte below a space, from 127 up, and the backslash written
 * `\xNN`, and a newline, as `msg_add_ext_text` does (kernel/printk/printk.c).
 *
 * The level and the stamp come from the record's own prefix, which the text
 * then leaves out (`nc_prefix`); the flags are `-`. A piece of a longer line
 * after its first has no prefix of its own, so it takes the last record's
 * level and stamp and the flag `c`, which Linux gives a continuation.
 */
static uint32_t nc_extend(struct nc_target *t, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t p = 0, i, j;
    char flag;

    if (n > 0 && nc_line[n - 1] == '\n') {
        n--;
    }
    i = nc_prefix(t, n);
    flag = i == 0 ? 'c' : '-';
    if (t->usec[0] == '\0') {
        t->usec[0] = '0';
    }
    if (t->release) {
        for (j = 0; UTS_RELEASE[j] != '\0'; j++) {
            nc_ext[p++] = UTS_RELEASE[j];
        }
        nc_ext[p++] = ',';
    }
    p += nc_put_u32(nc_ext + p, (uint32_t)t->level);
    nc_ext[p++] = ',';
    p += nc_put_u32(nc_ext + p, t->seq);
    nc_ext[p++] = ',';
    for (j = 0; t->usec[j] != '\0'; j++) {
        nc_ext[p++] = t->usec[j];
    }
    nc_ext[p++] = ',';
    nc_ext[p++] = flag;
    nc_ext[p++] = ';';
    for (; i < n; i++) {
        uint8_t c = (uint8_t)nc_line[i];

        if (c < ' ' || c >= 127 || c == '\\') {
            nc_ext[p++] = '\\';
            nc_ext[p++] = 'x';
            nc_ext[p++] = hex[c >> 4];
            nc_ext[p++] = hex[c & 0xF];
        } else {
            nc_ext[p++] = (char)c;
        }
    }
    nc_ext[p++] = '\n';
    return p;
}

/** Where the payload starts in `nc_pkt` for `t`. */
static uint32_t nc_payload_at(const struct nc_target *t)
{
    return (t->ipv6 ? NC_IP6_HDR : NC_IP4_HDR) + NC_UDP_HDR;
}

/**
 * Send the `plen` bytes at `nc_pkt + nc_payload_at(t)` to `t` from `src` as
 * one UDP datagram, built as `netpoll_send_udp` builds it. Returns what
 * `ethernet_tx` returns. Under `net_lock`.
 */
static int nc_udp_tx(struct nc_target *t, const uint8_t *src, uint32_t plen)
{
    uint32_t ip = t->ipv6 ? NC_IP6_HDR : NC_IP4_HDR;
    uint8_t *udp = nc_pkt + ip;
    uint32_t ulen = NC_UDP_HDR + plen;
    uint8_t pseudo[4];
    uint32_t sum;
    uint16_t check;
    int i;

    write16be(udp, t->local_port);
    write16be(udp + 2, t->remote_port);
    write16be(udp + 4, (uint16_t)ulen);
    write16be(udp + 6, 0);
    if (t->ipv6) {
        sum = nc_sum(0, src, 16);
        sum = nc_sum(sum, t->remote_ip, 16);
        write32be(pseudo, ulen);
        sum = nc_sum(sum, pseudo, 4);
        write32be(pseudo, IP_PROTO_UDP);
        sum = nc_sum(sum, pseudo, 4);
    } else {
        sum = nc_sum(0, src, 4);
        sum = nc_sum(sum, t->remote_ip, 4);
        pseudo[0] = 0;
        pseudo[1] = IP_PROTO_UDP;
        write16be(pseudo + 2, (uint16_t)ulen);
        sum = nc_sum(sum, pseudo, 4);
    }
    check = (uint16_t)(~nc_sum(sum, udp, ulen) & 0xFFFFu);
    write16be(udp + 6, check == 0 ? 0xFFFFu : check);

    if (t->ipv6) {
        nc_pkt[0] = 0x60;
        nc_pkt[1] = 0;
        nc_pkt[2] = 0;
        nc_pkt[3] = 0;
        write16be(nc_pkt + 4, (uint16_t)ulen);
        nc_pkt[6] = IP_PROTO_UDP;
        nc_pkt[7] = 32;
        for (i = 0; i < 16; i++) {
            nc_pkt[8 + i] = src[i];
            nc_pkt[24 + i] = t->remote_ip[i];
        }
        return ethernet_tx(t->nd, t->remote_mac, ETH_TYPE_IPV6, nc_pkt, (uint16_t)(ip + ulen));
    }
    nc_pkt[0] = 0x45;
    nc_pkt[1] = 0;
    write16be(nc_pkt + 2, (uint16_t)(ip + ulen));
    write16be(nc_pkt + 4, ++nc_ip_id);
    write16be(nc_pkt + 6, 0);
    nc_pkt[8] = 64;
    nc_pkt[9] = IP_PROTO_UDP;
    write16be(nc_pkt + 10, 0);
    for (i = 0; i < 4; i++) {
        nc_pkt[12 + i] = src[i];
        nc_pkt[16 + i] = t->remote_ip[i];
    }
    write16be(nc_pkt + 10, ip_checksum(nc_pkt, NC_IP4_HDR));
    return ethernet_tx(t->nd, t->remote_mac, ETH_TYPE_IP, nc_pkt, (uint16_t)(ip + ulen));
}

/**
 * Send the extended message of `len` bytes in `nc_ext` to `t`: whole when it
 * fits NETCONSOLE_CHUNK_MAX, and otherwise in pieces, each the header with
 * `,ncfrag=OFFSET/TOTAL` before its `;` and as much of the text after it as
 * fits, as Linux's `send_ext_msg_udp` sends one. Returns what the last
 * `nc_udp_tx` returned, stopping at the first refusal.
 */
static int nc_send_ext(struct nc_target *t, const uint8_t *src, uint32_t len)
{
    uint8_t *out = nc_pkt + nc_payload_at(t);
    uint32_t header, body, offset = 0, i;
    int r = NETDEV_TX_OK;

    if (len <= NETCONSOLE_CHUNK_MAX) {
        for (i = 0; i < len; i++) {
            out[i] = (uint8_t)nc_ext[i];
        }
        return nc_udp_tx(t, src, len);
    }
    for (header = 0; nc_ext[header] != ';'; header++) {
        /* the header is everything before the first semicolon */
    }
    body = len - header - 1;
    while (offset < body && r == NETDEV_TX_OK) {
        uint32_t p = header, chunk;

        for (i = 0; i < header; i++) {
            out[i] = (uint8_t)nc_ext[i];
        }
        out[p++] = ',';
        for (i = 0; "ncfrag="[i] != '\0'; i++) {
            out[p++] = (uint8_t)"ncfrag="[i];
        }
        p += nc_put_u32((char *)out + p, offset);
        out[p++] = '/';
        p += nc_put_u32((char *)out + p, body);
        out[p++] = ';';
        chunk = body - offset;
        if (chunk > NETCONSOLE_CHUNK_MAX - p) {
            chunk = NETCONSOLE_CHUNK_MAX - p;
        }
        for (i = 0; i < chunk; i++) {
            out[p + i] = (uint8_t)nc_ext[header + 1 + offset + i];
        }
        r = nc_udp_tx(t, src, p + chunk);
        offset += chunk;
    }
    return r;
}

/**
 * Send `t` from `src` the line Linux puts before a plain console's next record
 * when it lost `t->lost` (`console_prepend_dropped`, kernel/printk/
 * printk.c:2904-2906), in a datagram of its own. Returns what `nc_udp_tx`
 * returns. Under `net_lock`.
 */
static int nc_send_dropped(struct nc_target *t, const uint8_t *src)
{
    static const char before[] = "** ", after[] = " printk messages dropped **\n";
    char *out = (char *)nc_pkt + nc_payload_at(t);
    uint32_t p = 0, i;

    for (i = 0; before[i] != '\0'; i++) {
        out[p++] = before[i];
    }
    p += nc_put_u32(out + p, t->lost);
    for (i = 0; after[i] != '\0'; i++) {
        out[p++] = after[i];
    }
    return nc_udp_tx(t, src, p);
}

/**
 * Send `t` its next line before the place `end`, after what it lost: the
 * count added to the extended form's `seq`, which is then the record's own
 * number among those written since the target began, as Linux's is
 * (`info_print_ext_header`, kernel/printk/printk.c:662-680), or the plain
 * form's line from `nc_send_dropped`. Returns 1 when the line was sent, or
 * dropped as too long for the interface, which no later attempt would
 * change; 0 when there is none whole yet, or the drain must wait, with
 * `t->held` set. Under `net_lock`.
 */
static int nc_send_line(struct nc_target *t, uint32_t end)
{
    uint8_t src[16];
    uint32_t n, i, from;
    int r;

    if (!nc_link_ready(t->nd, &t->start, nc_behind(t->tail)) || !nc_source(t, src)) {
        t->held = 1;
        return 0;
    }
    nc_pass_gaps(t);
    n = nc_take(t->tail, end, t->extended ? NC_EXT_LINE : NETCONSOLE_CHUNK_MAX);
    if (n == 0) {
        return 0;
    }
    if (t->lost != 0 && !t->extended) {
        r = nc_send_dropped(t, src);
        if (r == NETDEV_TX_DROPPED) {
            t->held = 1;
            return 0;
        }
        if (r == NETDEV_TX_OK) {
            t->datagrams++;
        }
        t->lost = 0;
    }
    if (t->extended) {
        t->seq += t->lost;
        t->lost = 0;
        r = nc_send_ext(t, src, nc_extend(t, n));
    } else {
        for (i = 0; i < n; i++) {
            nc_pkt[nc_payload_at(t) + i] = (uint8_t)nc_line[i];
        }
        r = nc_udp_tx(t, src, n);
    }
    if (r == NETDEV_TX_DROPPED) {
        t->held = 1;
        return 0;
    }
    from = t->tail + n;
    t->tail = from >= NETCONSOLE_RING ? from - NETCONSOLE_RING : from;
    t->held = 0;
    t->seq++;
    if (r == NETDEV_TX_OK) {
        t->datagrams++;
    }
    return 1;
}

/** Send each target the whole lines the ring holds, as `netconsole_flush`
 * says; without the frames, `nc_tail` then follows the target furthest
 * behind. */
static void nc_flush_targets(void)
{
    uint32_t flags = net_lock();
    uint32_t end;
    struct nc_target *t;

    if (!nc_targets || nc_sending) {
        net_unlock(flags);
        return;
    }
    nc_sending = 1;
    end = nc_head;
    net_unlock(flags);

    for (t = nc_targets; t; t = t->next) {
        int sent;

        do {
            flags = net_lock();
            sent = nc_send_line(t, end);
            net_unlock(flags);
        } while (sent);
    }

    flags = net_lock();
    nc_follow();
    nc_retire_gaps();
    nc_sending = 0;
    net_unlock(flags);
}

void netconsole_flush(void)
{
    nc_flush_frames();
    nc_flush_targets();
}

void netconsole_tick(void)
{
    struct nc_target *t;
    int starting = nc_enabled && nc_frames_start.begun != 0;
    uint64_t now;

    for (t = nc_targets; t && !starting; t = t->next) {
        starting = t->start.begun != 0;
    }
    if (!starting || nc_sending) {
        return;
    }
    now = tick_count();
    if (now < nc_tick_next) {
        return;
    }
    nc_tick_next = now + MS_TO_TICKS(NC_START_POLL_MS);
    netconsole_flush();
}

void netconsole_shutdown(void)
{
    nc_final = 1;
    netconsole_flush();
}

void netconsole_panic(void)
{
    uint32_t flags = net_lock();

    nc_sending = 0;
    net_unlock(flags);
}

/* ---- reading netconsole= ---- */

/** The first `c` in `s`, or NULL. */
static char *nc_strchr(char *s, char c)
{
    for (; *s != '\0'; s++) {
        if (*s == c) {
            return s;
        }
    }
    return 0;
}

/** Whether `s` has a `c`. */
static int nc_has(const char *s, char c)
{
    for (; *s != '\0'; s++) {
        if (*s == c) {
            return 1;
        }
    }
    return 0;
}

/** Read `s` whole as a decimal port, as `kstrtou16` does; 0 when it is not
 * one. */
static int nc_parse_port(const char *s, uint16_t *out)
{
    uint32_t v = 0;

    if (*s == '+') {
        s++;
    }
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > 0xFFFFu) {
            return 0;
        }
    }
    *out = (uint16_t)v;
    return 1;
}

/**
 * Read `s` whole as an address, as Linux's `netpoll_parse_ip_addr` does: a
 * dotted IPv4 address when it has no colon, else IPv6. Returns 0 for IPv4,
 * 1 for IPv6, -1 for neither; `out` gets the address in network order.
 */
static int nc_parse_ip(const char *s, uint8_t *out)
{
    const char *p = s;
    uint32_t len = 0;
    int part;

    while (s[len] != '\0') {
        len++;
    }
    if (!nc_has(s, ':')) {
        for (part = 0; part < 4; part++) {
            uint32_t v = 0;
            int digits = 0;

            while (*p >= '0' && *p <= '9' && digits < 3) {
                v = v * 10u + (uint32_t)(*p++ - '0');
                digits++;
            }
            if (digits == 0 || v > 255 || *p != (part == 3 ? '\0' : '.')) {
                return -1;
            }
            out[part] = (uint8_t)v;
            p++;
        }
        return 0;
    }
    return ip6_parse(s, len, out) == 0 ? 1 : -1;
}

/** Read `s` as a station address, six pairs of hexadecimal digits separated
 * by colons, as Linux's `mac_pton` does; 0 when it is not one. */
static int nc_parse_mac(const char *s, uint8_t *out)
{
    int i, k;

    for (i = 0; i < 6; i++) {
        uint8_t v = 0;

        for (k = 0; k < 2; k++) {
            char c = s[i * 3 + k];

            if (c >= '0' && c <= '9') {
                v = (uint8_t)(v * 16 + (c - '0'));
            } else if (c >= 'a' && c <= 'f') {
                v = (uint8_t)(v * 16 + (c - 'a' + 10));
            } else if (c >= 'A' && c <= 'F') {
                v = (uint8_t)(v * 16 + (c - 'A' + 10));
            } else {
                return 0;
            }
        }
        if (i != 5 && s[i * 3 + 2] != ':') {
            return 0;
        }
        out[i] = v;
    }
    return 1;
}

/** The interface whose station address is `mac`, or NULL. */
static struct netdev *nc_netdev_by_mac(const uint8_t *mac)
{
    struct netdev *nd;
    int n, i;

    for (n = 0; (nd = netdev_at(n)) != 0; n++) {
        for (i = 0; i < 6 && nd->mac[i] == mac[i]; i++) {
            /* compare each byte */
        }
        if (i == 6) {
            return nd;
        }
    }
    return 0;
}

/** Where the interface of a target is named: by name, or by station address
 * when `by_mac` is set. */
struct nc_dev {
    const char *name;
    uint8_t mac[6];
    int by_mac;
};

/**
 * Read `cur`, a target past its `+` and `r`, into `t` and `dev` as Linux's
 * `netpoll_parse_options` reads one (net/core/netpoll.c), writing NULs over
 * its separators. Returns NULL, or where the text stopped making sense.
 */
static char *nc_parse_options(char *cur, struct nc_target *t, struct nc_dev *dev)
{
    char *delim;
    int ipversion_set = 0, v6;

    if (*cur != '@') {
        if ((delim = nc_strchr(cur, '@')) == 0) {
            return cur;
        }
        *delim = '\0';
        if (!nc_parse_port(cur, &t->local_port)) {
            return cur;
        }
        cur = delim;
    }
    cur++;
    if (*cur != '/') {
        ipversion_set = 1;
        if ((delim = nc_strchr(cur, '/')) == 0) {
            return cur;
        }
        *delim = '\0';
        if ((v6 = nc_parse_ip(cur, t->local_ip)) < 0) {
            return cur;
        }
        t->ipv6 = v6;
        cur = delim;
    }
    cur++;
    if (*cur != ',') {
        if ((delim = nc_strchr(cur, ',')) == 0) {
            return cur;
        }
        *delim = '\0';
        if (!nc_has(cur, ':')) {
            dev->name = cur;
        } else if (nc_parse_mac(cur, dev->mac)) {
            dev->by_mac = 1;
        } else {
            return cur;
        }
        cur = delim;
    }
    cur++;
    if (*cur != '@') {
        if ((delim = nc_strchr(cur, '@')) == 0) {
            return cur;
        }
        *delim = '\0';
        if (!nc_parse_port(cur, &t->remote_port)) {
            return cur;
        }
        cur = delim;
    }
    cur++;
    if ((delim = nc_strchr(cur, '/')) == 0) {
        return cur;
    }
    *delim = '\0';
    if ((v6 = nc_parse_ip(cur, t->remote_ip)) < 0 || (ipversion_set && t->ipv6 != v6)) {
        return cur;
    }
    t->ipv6 = v6;
    cur = delim + 1;
    if (*cur != '\0' && !nc_parse_mac(cur, t->remote_mac)) {
        return cur;
    }
    return 0;
}

/**
 * Read one target, `cur`, into `t` as Linux's `alloc_and_init` and
 * `alloc_param_target` read one (drivers/net/netconsole.c): Linux's ports,
 * `eth0` and the broadcast station address unless it names others, `+` and
 * `r` first. Then find its interface, which may be any card but not `lo`.
 * Returns 1, or 0 having logged why not.
 */
static int nc_parse_target(char *cur, struct nc_target *t)
{
    struct nc_dev dev;
    char *bad;
    int i;

    dev.name = NETDEV_IFNAME;
    dev.by_mac = 0;
    t->local_port = NETCONSOLE_LOCAL_PORT;
    t->remote_port = NETCONSOLE_REMOTE_PORT;
    for (i = 0; i < 6; i++) {
        t->remote_mac[i] = 0xFF;
    }
    if (*cur == '+') {
        t->extended = 1;
        cur++;
    }
    if (*cur == 'r') {
        if (!t->extended) {
            klog(KLOG_ERR, "netconsole", "refused: %s: r needs the extended form, +", cur);
            return 0;
        }
        t->release = 1;
        cur++;
    }
    if ((bad = nc_parse_options(cur, t, &dev)) != 0) {
        klog(KLOG_ERR, "netconsole", "refused: cannot read the target at '%s'", bad);
        return 0;
    }
    t->nd = dev.by_mac ? nc_netdev_by_mac(dev.mac) : netdev_by_name(dev.name);
    if (!t->nd || t->nd == netdev_loopback()) {
        klog(KLOG_ERR, "netconsole", "refused: %s: no such card",
             dev.by_mac ? "station address" : dev.name);
        return 0;
    }
    return 1;
}

/** Log the target `t` as Linux's `netpoll_print_options` gives one. */
static void nc_print_target(const struct nc_target *t)
{
    char local[40], remote[40];

    if (t->ipv6) {
        ip6_format(t->local_ip, local);
        ip6_format(t->remote_ip, remote);
    } else {
        local[0] = remote[0] = '\0';
    }
    if (t->ipv6) {
        klog(KLOG_INFO, "netconsole", "udp: %d@%s/%s,%d@%s/%X%X:%X%X:%X%X:%X%X:%X%X:%X%X%s%s",
             (int)t->local_port, nc_zero(t->local_ip, 16) ? "" : local, t->nd->name,
             (int)t->remote_port, remote, (unsigned)(t->remote_mac[0] >> 4),
             (unsigned)(t->remote_mac[0] & 0xF), (unsigned)(t->remote_mac[1] >> 4),
             (unsigned)(t->remote_mac[1] & 0xF), (unsigned)(t->remote_mac[2] >> 4),
             (unsigned)(t->remote_mac[2] & 0xF), (unsigned)(t->remote_mac[3] >> 4),
             (unsigned)(t->remote_mac[3] & 0xF), (unsigned)(t->remote_mac[4] >> 4),
             (unsigned)(t->remote_mac[4] & 0xF), (unsigned)(t->remote_mac[5] >> 4),
             (unsigned)(t->remote_mac[5] & 0xF), t->extended ? ", extended" : "",
             t->release ? " with the release" : "");
        return;
    }
    klog(KLOG_INFO, "netconsole",
         "udp: %d@%d.%d.%d.%d/%s,%d@%d.%d.%d.%d/%X%X:%X%X:%X%X:%X%X:%X%X:%X%X%s%s",
         (int)t->local_port, (int)t->local_ip[0], (int)t->local_ip[1], (int)t->local_ip[2],
         (int)t->local_ip[3], t->nd->name, (int)t->remote_port, (int)t->remote_ip[0],
         (int)t->remote_ip[1], (int)t->remote_ip[2], (int)t->remote_ip[3],
         (unsigned)(t->remote_mac[0] >> 4), (unsigned)(t->remote_mac[0] & 0xF),
         (unsigned)(t->remote_mac[1] >> 4), (unsigned)(t->remote_mac[1] & 0xF),
         (unsigned)(t->remote_mac[2] >> 4), (unsigned)(t->remote_mac[2] & 0xF),
         (unsigned)(t->remote_mac[3] >> 4), (unsigned)(t->remote_mac[3] & 0xF),
         (unsigned)(t->remote_mac[4] >> 4), (unsigned)(t->remote_mac[4] & 0xF),
         (unsigned)(t->remote_mac[5] >> 4), (unsigned)(t->remote_mac[5] & 0xF),
         t->extended ? ", extended" : "", t->release ? " with the release" : "");
}

/** Choose the boot id the frames carry. It tells boots apart in a capture
 * and need not be unpredictable; it is never 0. */
static void nc_choose_boot_id(void)
{
    nc_boot_id = krandom_u32();
    if (nc_boot_id == 0) {
        nc_boot_id = 1;
    }
}

/** Read `netpoll.carrier_timeout=`, in seconds, into `nc_carrier_ticks`, as
 * Linux's parameter of that name is read; a value that is not a decimal
 * number of seconds leaves Linux's default of 4. */
static void nc_read_carrier_timeout(void)
{
    char value[16];
    uint32_t v = 0, i, d;

    if (!kcmdline_value("netpoll.carrier_timeout", value, sizeof(value)) || value[0] == '\0') {
        return;
    }
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return;
        }
        d = (uint32_t)(value[i] - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) {
            return;
        }
        v = v * 10u + d;
    }
    nc_carrier_ticks = (uint64_t)v * TICK_HZ;
}

/**
 * The targets are read in order and kept in that order. They and the frames
 * start together, each at the oldest character the ring holds, so each is
 * sent the log from the boot's first record, as Linux's CON_PRINTBUFFER
 * replays the log buffer to a new console (`init_netconsole`). Started one at
 * a time, the first one's drain, which its own log line begins, would free
 * what the next had not been sent. Each begins its start here.
 */
int netconsole_setup(const char *config, int frames)
{
    static char copy[512];
    char *input = copy, *target;
    struct nc_target *first = 0, **link = &first, *t;
    uint32_t i, flags;
    int count = 0;

    if (nc_setup_done) {
        return 0;
    }
    nc_setup_done = 1;
    for (i = 0; i + 1 < sizeof(copy) && config[i] != '\0'; i++) {
        copy[i] = config[i];
    }
    copy[i] = '\0';

    while (input) {
        char *semi = nc_strchr(input, ';');

        target = input;
        if (semi) {
            *semi = '\0';
            input = semi + 1;
        } else {
            input = 0;
        }
        t = (struct nc_target *)kzalloc(sizeof(*t));
        if (!t) {
            klog(KLOG_ERR, "netconsole", "refused: no memory for a target");
            break;
        }
        if (!nc_parse_target(target, t)) {
            kfree(t);
            continue;
        }
        nc_print_target(t);
        *link = t;
        link = &t->next;
        count++;
    }
    if (count > 0) {
        klog(KLOG_INFO, "netconsole", "started: network logging, targets %d", count);
    }
    if (frames && !nc_enabled) {
        nc_choose_boot_id();
    }
    nc_read_carrier_timeout();

    flags = net_lock();
    for (t = first; t; t = t->next) {
        t->tail = nc_tail;
        t->gap = nc_gap_first;
        nc_begin(&t->start);
    }
    nc_targets = first;
    if (frames && !nc_enabled) {
        nc_begin(&nc_frames_start);
        nc_enabled = 1;
    }
    net_unlock(flags);
    netconsole_flush();
    return count;
}

void netconsole_enable(void)
{
    if (nc_enabled) {
        return;
    }
    nc_choose_boot_id();
    nc_read_carrier_timeout();
    nc_begin(&nc_frames_start);
    nc_enabled = 1;
    netconsole_flush();
}

int netconsole_is_enabled(void)
{
    return nc_enabled || nc_targets != 0;
}

void netconsole_print_info(void)
{
    struct nc_target *t;

    if (nc_enabled) {
        klog(KLOG_INFO, "netconsole",
             "broadcast, ethertype 0x%x, boot id 0x%x, "
             "%d frames, %d characters dropped",
             (int)NETCONSOLE_ETHERTYPE, (int)nc_boot_id, (int)nc_frames,
             (int)(nc_dropped + nc_frames_skipped));
    }
    for (t = nc_targets; t; t = t->next) {
        klog(KLOG_INFO, "netconsole", "udp: port %d of %s: %d datagrams%s, %d characters dropped",
             (int)t->remote_port, t->nd->name, (int)t->datagrams, t->held ? ", held" : "",
             (int)(nc_dropped + t->skipped));
    }
}
