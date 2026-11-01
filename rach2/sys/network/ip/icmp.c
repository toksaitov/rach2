/*
 * icmp.c: ICMP, as icmp.h describes it: echo requests answered, the
 * kernel's own ping, the destination unreachables this machine sends, and
 * the errors it takes. An echo message is, big-endian, type (1 byte), code
 * (1), checksum (2), identifier (2), sequence (2) and data.
 *
 * A process that pings sleeps on a wait queue that icmp_rx wakes, because
 * the timer does not switch away from a process inside a system call
 * (`sched_preempt`): a poll with sti and hlt inside a call would give no
 * other process the processor for as long as the wait lasts, so on a machine
 * several people can log into, any one of them could stop the rest. The
 * debug shell polls: it runs with the scheduler stopped and no process to
 * block, so there is nothing to yield to.
 *
 * The wait is bounded twice over, and both halves are necessary. The
 * process arms `wakeup_tick` so that the timer makes it runnable, since a
 * PROC_BLOCKED process nothing wakes is one schedule() never picks again;
 * and the deadline is also in the wait's condition, since the wait ends only
 * on a true condition or a signal it acts on: a sleeper whose condition is
 * still false waits again, its deadline armed again by `wq_wait`, and once
 * the deadline has passed the timer would wake it at every tick and the wait
 * would never end. Both read the one deadline word, so a timer that has
 * fired implies a condition that is true.
 *
 * There is one ping in flight for the whole machine; a program that needs
 * echo exchanges of its own opens a raw socket (socket/raw.c). The slot is
 * claimed under net_lock and released on every way out, and a second caller
 * is refused with -EBUSY and not queued, since a queue would need a fairness
 * rule and would leave a shell command waiting with nothing to show why.
 */

#include "../../klog.h"
#include "../../rtc.h"
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
#include "../../timer.h"
#include "../ip6/ip6.h"
#include "../ip6/icmp6.h"
#include "../../mm/kheap.h"

/* The echo reply being sent and the kernel's request, each sized for one
 * frame's datagram; a larger one is built in a block of the heap. */
#define ICMP_FRAME_MAX (1500 - 20)
static uint8_t icmp_reply_buf[ICMP_FRAME_MAX];
static uint8_t icmp_echo_buf[ICMP_FRAME_MAX];

/* An error quotes as much of the datagram it answers as fits a datagram of
 * 576 bytes (RFC 1812, 4.3.2.3), as Linux's does. */
#define ICMP_QUOTE_MAX (576 - 20 - 8)
static uint8_t icmp_error_buf[8 + ICMP_QUOTE_MAX];

/* Errors leave at most one a second after a burst of six, Linux's
 * icmp_ratelimit and XRLIM_BURST_FACTOR, kept here as a credit in ticks. */
#define ICMP_ERROR_TICKS TICK_HZ
#define ICMP_ERROR_BURST (6 * ICMP_ERROR_TICKS)
static uint32_t icmp_error_credit = ICMP_ERROR_BURST;
static uint32_t icmp_error_last;

/* The identifier of the kernel's own echo requests. */
#define ICMP_PING_ID 0x5243 /* 'RC' */

/* The wait for each request's answer, in ticks, and the requests sent. */
#define ICMP_PING_WAIT     TICK_HZ /* 1 s */
#define ICMP_PING_ATTEMPTS 4

/* The one ping in flight, which an echo reply is matched against: whether
 * it is in flight and answered, the answer's length and hop limit, the
 * sequence and peer it waits for, an IPv4 address or, when `ping_v6` is
 * set, the IPv6 one in `ping_peer6`, and the tick its wait ends at, never 0;
 * `ping_seq` is the sequence of the next request. */
static volatile int ping_active = 0;
static volatile int ping_replied = 0;
static volatile uint16_t ping_reply_len; /* the ICMP bytes of the reply */
static volatile uint8_t ping_reply_ttl;  /* the hop limit it arrived with */
static volatile uint16_t ping_seq_wait = 0;
static volatile uint32_t ping_peer_ip = 0;
static volatile int ping_v6 = 0;
static uint8_t ping_peer6[16];
static volatile uint64_t ping_deadline = 0;
static uint16_t ping_seq = 0;

/* Where a sleeping pinger waits, and what icmp_rx wakes; initialized by the
 * first ping. */
static wait_queue_t ping_wq;
static int ping_wq_ready = 0;

/**
 * Take a router's report that a datagram from this machine was too big for
 * a link, whose MTU is at bytes 6 and 7. The report quotes the datagram's IP
 * header and at least its first eight bytes (RFC 792). The quoted
 * destination's path MTU falls to the link's (`ip_pmtu_lower`), whatever
 * the protocol, when the quoted source is this machine's. For a TCP
 * segment the quote also gives the connection's ports and the segment's
 * sequence number, which `tcp_path_mtu` finds the connection by. A quote
 * too short for its header is ignored.
 */
static void icmp_too_big(const uint8_t *data, uint16_t len)
{
    const uint8_t *ip = data + 8;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    const uint8_t *tcp = ip + ihl;

    if (ihl < 20 || 8u + ihl > len || !netdev_owns(read32be(ip + 12))) {
        return;
    }
    ip_pmtu_lower(read32be(ip + 16), read16be(data + 6));
    if (8u + ihl + 8u > len || ip[9] != IP_PROTO_TCP) {
        return;
    }
    tcp_path_mtu(read32be(ip + 12), read32be(ip + 16), read16be(tcp), read16be(tcp + 2),
                 read32be(tcp + 4), read16be(data + 6));
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
 * Pass a port unreachable to UDP: the datagram it quotes says which of this
 * machine's ports sent it and to which peer. A quote too short to hold the
 * ports, or of another protocol, is ignored.
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
    uint32_t now = (uint32_t)kernel_ticks;
    uint32_t credit = icmp_error_credit + (now - icmp_error_last);

    if (credit > ICMP_ERROR_BURST || credit < icmp_error_credit) {
        credit = ICMP_ERROR_BURST;
    }
    icmp_error_last = now;
    if (credit < ICMP_ERROR_TICKS) {
        icmp_error_credit = credit;
        return 0;
    }
    icmp_error_credit = credit - ICMP_ERROR_TICKS;
    return 1;
}

/** Send the error of `type` and `code` about `dgram`, `len` bytes, to its
 * source, as `icmp_unreachable` says. */
static void icmp_error(uint8_t type, uint8_t code, const uint8_t *dgram, uint16_t len)
{
    uint32_t src = read32be(dgram + 12);
    uint16_t quote = len < ICMP_QUOTE_MAX ? len : ICMP_QUOTE_MAX;

    if (src == 0 || (src & 0xF0000000u) == 0xE0000000u || netdev_is_broadcast(src) ||
        (!ip_is_local(src) && !icmp_error_allowed())) {
        return;
    }
    icmp_error_buf[0] = type;
    icmp_error_buf[1] = code;
    write16be(icmp_error_buf + 2, 0);
    write32be(icmp_error_buf + 4, 0);
    k_memcpy(icmp_error_buf + 8, dgram, quote);
    write16be(icmp_error_buf + 2, ip_checksum(icmp_error_buf, (uint16_t)(8 + quote)));
    if (ip_tx(src, IP_PROTO_ICMP, icmp_error_buf, (uint16_t)(8 + quote), IP_TTL_DEFAULT) == 0) {
        icmp_count_out(type);
    }
}

void icmp_unreachable(const uint8_t *dgram, uint16_t len, uint8_t code)
{
    icmp_error(ICMP_UNREACHABLE, code, dgram, len);
}

void icmp_time_exceeded(const uint8_t *dgram, uint16_t len, uint8_t code)
{
    icmp_error(ICMP_TIME_EXCEEDED, code, dgram, len);
}

/**
 * An echo request is copied whole, its type changed to reply and its
 * checksum recomputed; one past a frame, which came in fragments, is
 * answered from a block of the heap, and in fragments, or not at all with
 * no memory. A fragmentation-needed report shorter than 36 bytes, the ICMP
 * header, a 20-byte IP header and eight bytes of the segment, is ignored.
 */
void icmp_rx(uint32_t src_ip, uint32_t dst_ip, uint8_t ttl, const uint8_t *data, uint16_t len)
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
        uint8_t *reply = len > sizeof(icmp_reply_buf) ? kmalloc(len) : icmp_reply_buf;

        net_mib.icmp_in_echos++;
        if (!reply) {
            return;
        }
        k_memcpy(reply, data, len);
        reply[0] = ICMP_ECHO_REPLY;
        reply[1] = 0;
        write16be(reply + 2, 0x0000);
        write16be(reply + 2, ip_checksum(reply, len));
        if (ip_tx_from(dst_ip, src_ip, IP_PROTO_ICMP, reply, len, IP_TTL_DEFAULT) == 0) {
            icmp_count_out(ICMP_ECHO_REPLY);
        }
        if (reply != icmp_reply_buf) {
            kfree(reply);
        }
    } else if (data[0] == ICMP_UNREACHABLE && data[1] == ICMP_FRAG_NEEDED && len >= 36) {
        icmp_too_big(data, len);
    } else if (data[0] == ICMP_UNREACHABLE && data[1] == ICMP_PORT_UNREACH) {
        icmp_refused(data, len);
    } else if (data[0] == ICMP_ECHO_REPLY) {
        uint16_t id = read16be(data + 4);
        uint16_t seq = read16be(data + 6);

        net_mib.icmp_in_echo_reps++;
        if (ping_active && !ping_v6 && src_ip == ping_peer_ip && id == ICMP_PING_ID &&
            seq == ping_seq_wait) {
            ping_reply_len = len;
            ping_reply_ttl = ttl;
            ping_replied = 1;
            if (ping_wq_ready) {
                wq_wake_all(&ping_wq);
            }
        }
    }
}

/**
 * A reply to a request sent to a group may come from any member, so the
 * peer is matched only when the request went to one host.
 */
void icmp_ping_reply6(const uint8_t *src, uint16_t id, uint16_t seq, uint16_t len, uint8_t hlim)
{
    if (ping_active && ping_v6 && id == ICMP_PING_ID && seq == ping_seq_wait &&
        (ip6_is_multicast(ping_peer6) || ip6_equal(src, ping_peer6))) {
        ping_reply_len = len;
        ping_reply_ttl = hlim;
        ping_replied = 1;
        if (ping_wq_ready) {
            wq_wake_all(&ping_wq);
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

    return ping_replied || tick_count() >= ping_deadline;
}

/**
 * Release the slot of the ping in flight. Called on every way out of
 * `icmp_ping_data` once it has the slot.
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
 * The attempts of `ping_run`, each request built in `echo`, which holds
 * 8 + `size` bytes.
 *
 * The slot is claimed for the whole call and released as its last act on
 * every path, so a killed pinger cannot strand it: the signal makes the
 * blocked process runnable, the wait breaks with -EINTR, and the release
 * runs before the system call returns. Each request is built and sent under
 * `net_lock`, and its deadline, a second's ticks past the count, is never
 * 0, `wakeup_tick`'s "not sleeping" value. Whether a
 * reply came is read from `ping_replied`, since a wait that returns does not
 * say whether the reply or the deadline ended it.
 */
static int ping_attempts(uint32_t dst_ip, const uint8_t *dst6, int ifindex, uint32_t size,
                         uint8_t ttl, uint16_t *got, uint8_t *got_ttl, uint8_t *echo)
{
    uint64_t start;
    uint32_t flags, i;
    uint16_t seq;
    int attempt;
    int rc = -ETIMEDOUT;

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
    ping_v6 = dst6 != 0;
    if (dst6) {
        k_memcpy(ping_peer6, dst6, sizeof(ping_peer6));
    }
    net_unlock(flags);

    for (attempt = 0; attempt < ICMP_PING_ATTEMPTS; attempt++) {
        seq = ping_seq++;

        flags = net_lock();

        echo[0] = dst6 ? ICMP6_ECHO_REQUEST : ICMP_ECHO_REQUEST;
        echo[1] = 0;
        write16be(echo + 2, 0x0000);
        write16be(echo + 4, ICMP_PING_ID);
        write16be(echo + 6, seq);
        for (i = 0; i < size; i++) {
            echo[8 + i] = (uint8_t)i;
        }
        if (!dst6) {
            write16be(echo + 2, ip_checksum(echo, (uint16_t)(8 + size)));
        }

        ping_peer_ip = dst_ip;
        ping_seq_wait = seq;
        ping_replied = 0;
        start = rtc_ticks_now();
        ping_deadline = start + ICMP_PING_WAIT;

        if (dst6) {
            int sent = icmp6_send(0, dst6, ifindex, echo, (uint16_t)(8 + size), ttl);

            if (sent < 0 && attempt == 0) {
                net_unlock(flags);
                ping_release();
                return sent;
            }
        } else if (ip_tx(dst_ip, IP_PROTO_ICMP, echo, (uint16_t)(8 + size), ttl) == 0) {
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
            while (!ping_replied && tick_count() < ping_deadline) {
                idle_wait_irq_on();
            }
        }

        if (ping_replied) {
            rc = (int)(rtc_ticks_now() - start);
            if (got) {
                *got = ping_reply_len;
                *got_ttl = ping_reply_ttl;
            }
            ping_release();

            return rc;
        }
    }

    ping_release();
    klog(KLOG_INFO, "icmp", "ping: no reply before the timeout");

    return rc;
}

/**
 * The echo of `icmp_ping_data` and `icmp_ping6_data`: to the IPv4 address
 * `dst_ip`, or with `dst6` not NULL to that IPv6 address in the scope of
 * `ifindex`. Returns as they do, and -ENOMEM when an IPv6 request past
 * `echo` finds no block for it. Called with interrupts on.
 */
static int ping_run(uint32_t dst_ip, const uint8_t *dst6, int ifindex, uint32_t size, uint8_t ttl,
                    uint16_t *got, uint8_t *got_ttl)
{
    uint8_t *echo = icmp_echo_buf;
    int rc;

    if (size > (dst6 ? ICMP6_PING_DATA_MAX : ICMP_PING_DATA_MAX)) {
        return -EMSGSIZE;
    }
    if (8 + size > sizeof(icmp_echo_buf)) {
        echo = kmalloc(8 + size);
        if (!echo) {
            return -ENOMEM;
        }
    }
    rc = ping_attempts(dst_ip, dst6, ifindex, size, ttl, got, got_ttl, echo);
    if (echo != icmp_echo_buf) {
        kfree(echo);
    }
    return rc;
}

int icmp_ping_data(uint32_t dst_ip, uint32_t size, uint8_t ttl, uint16_t *got, uint8_t *got_ttl)
{
    return ping_run(dst_ip, 0, 0, size, ttl, got, got_ttl);
}

/** An IPv6 request's first send that fails, for want of a route, a source
 * or a neighbour, fails the ping at once: no later attempt would fare
 * better, and a wait would only hide the reason. */
int icmp_ping6_data(const uint8_t *dst, int ifindex, uint32_t size, uint8_t hlim, uint16_t *got,
                    uint8_t *got_hlim)
{
    return ping_run(0, dst, ifindex, size, hlim, got, got_hlim);
}

int icmp_ping(uint32_t dst_ip)
{
    return icmp_ping_data(dst_ip, 0, IP_TTL_DEFAULT, 0, 0);
}
