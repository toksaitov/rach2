/*
 * icmp.c - Internet Control Message Protocol (echo only).
 *
 * Echo format (big-endian): type(1) code(1) checksum(2) id(2) seq(2) data(N).
 *
 * Receiving an echo request: copy it back, flip the type to reply, recompute
 * the checksum, and send. Receiving an echo reply: if it matches the ping in
 * flight (source, id, and sequence), mark it answered.
 *
 * icmp_ping sends a request and waits for the reply. A process waits by
 * sleeping on a wait queue that icmp_rx wakes, because the timer skips
 * schedule() while in_syscall is set: a poll with sti/hlt from inside a syscall
 * gives no other process any CPU at all for as long as the wait lasts, so on a
 * machine several people can log into, any one of them could stop the rest.
 * The boot shell polls: it runs with the scheduler stopped and no process to
 * block, so there is nothing to yield to.
 *
 * The wait is bounded twice over, and both halves are necessary. The
 * process arms wakeup_tick so the timer makes it runnable, since a BLOCKED
 * process nothing wakes is one schedule() never picks again; and the deadline
 * is also in the wait predicate, since the wait ends only on a true condition
 * or a signal it acts on: a sleeper whose condition is still false waits
 * again, its deadline armed again by `wq_wait`, and once the deadline has
 * passed the timer would wake it at every tick and the wait would never end.
 * Both read the one deadline word, so a timer that has fired implies a
 * predicate that is true.
 *
 * There is one in-flight request for the whole machine; a program that needs
 * echo exchanges of its own opens a raw socket (socket/raw.c). The slot is
 * claimed under net_lock and released on every exit path, and a second caller
 * is refused with -EBUSY rather than queued, since a queue would need a
 * fairness rule and would leave a shell command waiting with nothing to show
 * why.
 *
 * The first request on a cold ARP cache is dropped, so we retry a few times:
 * the early attempt warms the cache and a later one gets through.
 */

#include "../../klog.h"
#include "icmp.h"
#include "ip.h"
#include "../net.h"
#include "../core/byteorder.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../proc/process.h"
#include "../../proc/wait_queue.h"
#include "../../lib/kerrno.h"
#include "../../proc/sched.h"
#include "../tcp/tcp.h"
#include "../driver/netdev.h"
#include "../udp/udp.h"

/* Outgoing echo reply (sized for a full datagram) and request buffers. */
static uint8_t icmp_reply_buf[1500];
static uint8_t icmp_echo_buf[8 + ICMP_PING_DATA_MAX];

/* An error quotes as much of the datagram it answers as fits a datagram of
 * 576 bytes (RFC 1812, 4.3.2.3), as Linux's does. */
#define ICMP_QUOTE_MAX (576 - 20 - 8)
static uint8_t icmp_error_buf[8 + ICMP_QUOTE_MAX];

/* Errors leave at most one a second after a burst of six, Linux's
 * icmp_ratelimit and XRLIM_BURST_FACTOR, kept here as a credit in ticks. */
#define ICMP_ERROR_TICKS 100
#define ICMP_ERROR_BURST (6 * ICMP_ERROR_TICKS)
static uint32_t icmp_error_credit = ICMP_ERROR_BURST;
static uint32_t icmp_error_last;

/* Identifier we stamp on our own echo requests. */
#define ICMP_PING_ID 0x5243 /* 'RC' */

/* Per-attempt timeout (ticks) and number of attempts. */
#define ICMP_PING_WAIT     100 /* 1 s at 100 Hz */
#define ICMP_PING_ATTEMPTS 4

/* The one in-flight ping, matched against incoming replies. */
static volatile int ping_active = 0;
static volatile int ping_replied = 0;
static volatile uint16_t ping_reply_len; /* the ICMP bytes of the reply */
static volatile uint16_t ping_seq_wait = 0;
static volatile uint32_t ping_peer_ip = 0;
static volatile uint32_t ping_deadline = 0;
static uint16_t ping_seq = 0;

/* Where a sleeping pinger waits, and what icmp_rx wakes. */
static wait_queue_t ping_wq;
static int ping_wq_ready = 0;

/* Kernel tick counter, for the timeout. */
extern volatile uint32_t kernel_ticks;

/**
 * A router's report that a datagram of ours was too big for a link with the
 * MTU at bytes 6 and 7, which quotes the datagram's IP header and at least
 * its first eight bytes (RFC 792): for a TCP segment, the connection's
 * ports and the segment's sequence number, which the connection keeps the
 * MTU for (`tcp_path_mtu`).
 */
static void icmp_too_big(const uint8_t *data, uint16_t len)
{
    const uint8_t *ip = data + 8;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    const uint8_t *tcp = ip + ihl;

    if (ihl < 20 || 8u + ihl + 8u > len || ip[9] != IP_PROTO_TCP) {
        return;
    }
    tcp_path_mtu(read32be(ip + 16), read16be(tcp), read16be(tcp + 2), read32be(tcp + 4),
                 read16be(data + 6));
}

void icmp_count_out(uint8_t type)
{
    net_mib.icmp_out_msgs++;
    if (type == ICMP_UNREACHABLE) {
        net_mib.icmp_out_dest_unreachs++;
    } else if (type == ICMP_ECHO_REQUEST) {
        net_mib.icmp_out_echos++;
    } else if (type == ICMP_ECHO_REPLY) {
        net_mib.icmp_out_echo_reps++;
    }
}

/**
 * Pass a port unreachable to UDP: the datagram it quotes says which of our
 * ports sent it and to which peer. A quote too short to hold the ports, or of
 * another protocol, is ignored.
 */
static void icmp_refused(const uint8_t *data, uint16_t len)
{
    const uint8_t *q = data + 8;
    uint16_t ihl;

    if (len < 8 + 20) {
        return;
    }
    ihl = (uint16_t)((q[0] & 0x0F) * 4);
    if (ihl < 20 || len < 8 + ihl + 4 || q[9] != IP_PROTO_UDP) {
        return;
    }
    udp_refused(read16be(q + ihl), read32be(q + 16), read16be(q + ihl + 2));
}

/**
 * Whether the rate limit lets an error leave now, taking its credit if so.
 *
 * The credit grows by a tick each tick, up to the burst, and an error costs
 * a second of it. Linux keeps a credit for each peer; this one is the
 * machine's, so a peer that spends it delays the errors to every other.
 */
static int icmp_error_allowed(void)
{
    uint32_t credit = icmp_error_credit + (kernel_ticks - icmp_error_last);

    if (credit > ICMP_ERROR_BURST || credit < icmp_error_credit) {
        credit = ICMP_ERROR_BURST;
    }
    icmp_error_last = kernel_ticks;
    if (credit < ICMP_ERROR_TICKS) {
        icmp_error_credit = credit;
        return 0;
    }
    icmp_error_credit = credit - ICMP_ERROR_TICKS;
    return 1;
}

/**
 * Send a destination unreachable for `dgram`, quoting its header and as much
 * of the rest as fits, which `traceroute` reads to match the answer to its
 * probe and a UDP client to its socket.
 *
 * The caller has checked that the datagram was addressed to this machine and
 * not broadcast. No error goes to a source of 0.0.0.0, a broadcast or a
 * group, which is no single host, and none past the rate limit, which an
 * error to this machine's own address is not held to, as Linux does not
 * limit the errors it sends over its loopback.
 */
void icmp_unreachable(const uint8_t *dgram, uint16_t len, uint8_t code)
{
    uint32_t src = read32be(dgram + 12);
    uint16_t quote = len < ICMP_QUOTE_MAX ? len : ICMP_QUOTE_MAX;

    if (src == 0 || (src & 0xF0000000u) == 0xE0000000u || netdev_is_broadcast(src) ||
        (!netdev_owns(src) && !icmp_error_allowed())) {
        return;
    }
    icmp_error_buf[0] = ICMP_UNREACHABLE;
    icmp_error_buf[1] = code;
    write16be(icmp_error_buf + 2, 0);
    write32be(icmp_error_buf + 4, 0);
    k_memcpy(icmp_error_buf + 8, dgram, quote);
    write16be(icmp_error_buf + 2, ip_checksum(icmp_error_buf, (uint16_t)(8 + quote)));
    if (ip_tx(src, IP_PROTO_ICMP, icmp_error_buf, (uint16_t)(8 + quote), IP_TTL_DEFAULT) == 0) {
        icmp_count_out(ICMP_UNREACHABLE);
    }
}

/**
 * Handle one received ICMP datagram: answer echo requests, match echo replies.
 *
 * An echo request is reflected back to its source: the payload is copied
 * verbatim, the type is changed to reply, and the checksum is recomputed.
 * An echo reply is accepted only if it matches the single ping in flight by
 * source address, identifier, and sequence; a match sets `ping_replied`. A
 * report that a TCP segment was too big for a link on its path goes to
 * `icmp_too_big`.
 *
 * @param src_ip source address of the datagram, host byte order.
 * @param len ICMP length; datagrams shorter than the 8-byte header are dropped.
 */
void icmp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len)
{
    net_mib.icmp_in_msgs++;
    if (len < 8) {
        net_mib.icmp_in_errors++;
        return;
    }

    if (data[0] == ICMP_UNREACHABLE) {
        net_mib.icmp_in_dest_unreachs++;
    }
    if (data[0] == ICMP_ECHO_REQUEST) {
        net_mib.icmp_in_echos++;
        if (len > 1500) {
            len = 1500;
        }
        k_memcpy(icmp_reply_buf, data, len);
        icmp_reply_buf[0] = ICMP_ECHO_REPLY;
        icmp_reply_buf[1] = 0;
        write16be(icmp_reply_buf + 2, 0x0000);
        write16be(icmp_reply_buf + 2, ip_checksum(icmp_reply_buf, len));
        if (ip_tx_from(dst_ip, src_ip, IP_PROTO_ICMP, icmp_reply_buf, len, IP_TTL_DEFAULT) == 0) {
            icmp_count_out(ICMP_ECHO_REPLY);
        }
    } else if (data[0] == ICMP_UNREACHABLE && data[1] == ICMP_FRAG_NEEDED && len >= 36) {
        icmp_too_big(data, len);
    } else if (data[0] == ICMP_UNREACHABLE && data[1] == ICMP_PORT_UNREACH) {
        icmp_refused(data, len);
    } else if (data[0] == ICMP_ECHO_REPLY) {
        uint16_t id = read16be(data + 4);
        uint16_t seq = read16be(data + 6);

        net_mib.icmp_in_echo_reps++;
        if (ping_active && src_ip == ping_peer_ip && id == ICMP_PING_ID && seq == ping_seq_wait) {
            ping_reply_len = len;
            ping_replied = 1;
            if (ping_wq_ready) {
                wq_wake_all(&ping_wq);
            }
        }
    }
}

/**
 * Whether the ping in flight is done: answered, or past its deadline.
 *
 * Both halves are here on purpose. `wq_wait_event` re-tests this after every
 * wake and waits again while it is false, with the deadline armed again, so a
 * predicate that read only `ping_replied` would be woken at every tick once
 * the deadline had passed and would never return.
 */
static int ping_done(void *arg)
{
    (void)arg;

    return ping_replied || (int32_t)(kernel_ticks - ping_deadline) >= 0;
}

/**
 * Release the in-flight slot. Called on every exit path from `icmp_ping`.
 *
 * Under `net_lock` because `icmp_rx` reads the same words from an interrupt,
 * and it clears the arming too: a deadline left armed wakes this process early
 * out of its next unrelated sleep, since `nanosleep` and `poll` read the same
 * `wakeup_tick`.
 */
static void ping_release(void)
{
    uint32_t flags = net_lock();

    ping_active = 0;
    net_unlock(flags);
    if (current_proc) {
        current_proc->wakeup_tick = 0;
    }
}

/**
 * Send an ICMP echo request to `dst_ip` and wait for the matching reply.
 *
 * Claims the machine's one in-flight slot for the whole call and releases it as
 * its last act on every path, so a killed pinger cannot strand it: the signal
 * makes the blocked process runnable, the wait breaks with `-EINTR`, and the
 * release runs before the syscall returns. A caller that finds the slot held
 * sends nothing and gets `-EBUSY`.
 *
 * A process sleeps for the reply; the boot shell, which has no process and no
 * scheduler, polls with sti/hlt. The first request on a cold ARP cache is
 * dropped, so up to `ICMP_PING_ATTEMPTS` are sent; an early attempt warms the
 * cache. Each request is built and sent under `net_lock`, and its deadline is
 * moved off 0, `wakeup_tick`'s "not sleeping" value, as `sys_nanosleep` moves
 * its own: a deadline of 0 would arm nothing, and the sleeper would never be
 * made runnable again. Whether a reply came is read from `ping_replied`, since
 * a wait that returns does not say whether the reply or the deadline ended it.
 *
 * The request carries `size` bytes of data, each its own offset in the data
 * modulo 256, as iputils fills them, and leaves with the hop limit `ttl`.
 * The reply's ICMP length, which a peer may make shorter or longer than the
 * request, goes to `*got` when `got` is not NULL.
 *
 * @return round-trip time in ticks, `-ETIMEDOUT` if every attempt expired,
 *         `-EBUSY` if another ping has the slot, or `-EINTR` on a signal.
 */
int icmp_ping_data(uint32_t dst_ip, uint32_t size, uint8_t ttl, uint16_t *got)
{
    uint32_t start, flags, i;
    uint16_t seq;
    int attempt;
    int rc = -ETIMEDOUT;

    if (size > ICMP_PING_DATA_MAX) {
        return -EMSGSIZE;
    }

    if (!ping_wq_ready) {
        wait_queue_init(&ping_wq);
        ping_wq_ready = 1;
    }

    flags = net_lock();
    if (ping_active) {
        net_unlock(flags);

        return -EBUSY;
    }
    ping_active = 1;
    ping_replied = 0;
    net_unlock(flags);

    for (attempt = 0; attempt < ICMP_PING_ATTEMPTS; attempt++) {
        seq = ping_seq++;

        flags = net_lock();

        icmp_echo_buf[0] = ICMP_ECHO_REQUEST;
        icmp_echo_buf[1] = 0;
        write16be(icmp_echo_buf + 2, 0x0000);
        write16be(icmp_echo_buf + 4, ICMP_PING_ID);
        write16be(icmp_echo_buf + 6, seq);
        for (i = 0; i < size; i++) {
            icmp_echo_buf[8 + i] = (uint8_t)i;
        }
        write16be(icmp_echo_buf + 2, ip_checksum(icmp_echo_buf, (uint16_t)(8 + size)));

        ping_peer_ip = dst_ip;
        ping_seq_wait = seq;
        ping_replied = 0;
        start = kernel_ticks;
        ping_deadline = start + ICMP_PING_WAIT;
        if (ping_deadline == 0) {
            /* 0 is `wakeup_tick`'s "not sleeping" sentinel. */
            ping_deadline = 1;
        }

        if (ip_tx(dst_ip, IP_PROTO_ICMP, icmp_echo_buf, (uint16_t)(8 + size), ttl) == 0) {
            icmp_count_out(ICMP_ECHO_REQUEST);
        }

        net_unlock(flags);

        if (current_proc && current_proc->in_syscall) {
            int waited;

            current_proc->wakeup_tick = ping_deadline;
            waited = wq_wait_event(&ping_wq, ping_done, 0);
            current_proc->wakeup_tick = 0;
            if (waited < 0) {
                ping_release();

                return waited;
            }
        } else {
            while (!ping_replied && (int32_t)(kernel_ticks - ping_deadline) < 0) {
                idle_wait_irq_on();
            }
        }

        if (ping_replied) {
            rc = (int)(kernel_ticks - start);
            if (got) {
                *got = ping_reply_len;
            }
            ping_release();

            return rc;
        }
    }

    ping_release();
    klog(KLOG_INFO, "icmp", "ping: no reply before the timeout");

    return rc;
}

int icmp_ping(uint32_t dst_ip)
{
    return icmp_ping_data(dst_ip, 0, IP_TTL_DEFAULT, 0);
}
