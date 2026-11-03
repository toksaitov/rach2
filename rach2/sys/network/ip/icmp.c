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
 * Each caller of the kernel's ping has a record of its own, on its own
 * stack, linked on a list for as long as its call lasts, with an identifier
 * no other caller and no ICMP datagram socket holds (`icmp_echo_id`), so any
 * number of pings run at once and each reply ends only its own caller's
 * wait. A program that sends echo requests of its own opens an ICMP
 * datagram socket (socket/raw.c), as Linux's ping does.
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
#include "../socket/raw.h"

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

/* The wait for each request's answer, in ticks, and the requests sent. */
#define ICMP_PING_WAIT     TICK_HZ /* 1 s */
#define ICMP_PING_ATTEMPTS 4

/* A ping in flight, which an echo reply is matched against: its identifier,
 * the sequence and peer it waits for, an IPv4 address or, when `v6` is set,
 * the IPv6 one in `peer6`, and the tick its wait ends at, never 0; whether
 * it is answered, and the answer's length and hop limit; and the queue its
 * caller sleeps on. The record is the caller's, on its stack, and is on the
 * list `pingers` while the call lasts; the fields the receive path writes
 * are written under `net_lock` or with interrupts off. */
struct icmp_pinger {
    struct icmp_pinger *next;
    uint16_t id;
    uint16_t seq_wait;
    int v6;
    uint32_t peer_ip;
    uint8_t peer6[16];
    volatile uint64_t deadline;
    volatile int replied;
    volatile uint16_t reply_len; /* the ICMP bytes of the reply */
    volatile uint8_t reply_ttl;  /* the hop limit it arrived with */
    wait_queue_t wq;
};

static struct icmp_pinger *pingers;

/* Where the search for a free identifier starts next: 'RC', and then one
 * past the last identifier handed out. */
static uint16_t echo_id_next = 0x5243;

int icmp_ping_id_used(uint16_t id)
{
    struct icmp_pinger *p;

    for (p = pingers; p && p->id != id; p = p->next) {
    }
    return p != NULL;
}

uint16_t icmp_echo_id(void)
{
    uint32_t tries;

    for (tries = 0; tries < 0x10000u; tries++) {
        uint16_t id = echo_id_next++;

        if (!icmp_ping_id_used(id) && !raw_echo_id_used(id)) {
            return id;
        }
    }
    return echo_id_next++;
}

/**
 * Take a router's report that a datagram from this machine was too big for
 * a link, whose MTU is at bytes 6 and 7. The report quotes the datagram's IP
 * header and at least its first eight bytes (RFC 792). The quoted
 * destination's path MTU falls to the link's (`ip_pmtu_lower`), whatever
 * the protocol, when the quoted source is this machine's. For a TCP
 * segment the quote also gives the connection's ports and the segment's
 * sequence number, and the report changes nothing unless that sequence is
 * in flight on the connection (`tcp_in_flight`); then it also goes to
 * `tcp_path_mtu`. A quote too short for its header, or for a TCP segment's
 * first eight bytes, is ignored.
 */
static void icmp_too_big(const uint8_t *data, uint16_t len)
{
    const uint8_t *ip = data + 8;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    const uint8_t *tcp = ip + ihl;

    if (ihl < 20 || 8u + ihl > len || !netdev_owns(read32be(ip + 12))) {
        return;
    }
    if (ip[9] == IP_PROTO_TCP &&
        (8u + ihl + 8u > len ||
         !tcp_in_flight(net_addr4(read32be(ip + 12)), read16be(tcp), net_addr4(read32be(ip + 16)),
                        read16be(tcp + 2), read32be(tcp + 4)))) {
        return;
    }
    ip_pmtu_lower(read32be(ip + 16), read16be(data + 6));
    if (ip[9] != IP_PROTO_TCP) {
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
 * Give an echo reply to the ping in flight that waits for it: over IPv4
 * from `src_ip` when `src6` is null, or over IPv6 from the 16 bytes at
 * `src6`, with the identifier `id` and sequence `seq`, `len` ICMP bytes that
 * arrived with the hop limit `ttl`. A reply no caller waits for is
 * ignored. Called with interrupts off.
 */
static void ping_answer(uint32_t src_ip, const uint8_t *src6, uint16_t id, uint16_t seq,
                        uint16_t len, uint8_t ttl)
{
    struct icmp_pinger *p;
    int v6 = src6 != 0;

    for (p = pingers; p; p = p->next) {
        if (p->id != id || p->seq_wait != seq || p->v6 != v6) {
            continue;
        }
        if (v6 ? !ip6_is_multicast(p->peer6) && !ip6_equal(src6, p->peer6) : src_ip != p->peer_ip) {
            continue;
        }
        p->reply_len = len;
        p->reply_ttl = ttl;
        p->replied = 1;
        wq_wake_all(&p->wq);
        return;
    }
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
        ping_answer(src_ip, 0, id, seq, len, ttl);
    }
}

/** A reply to a request sent to a group may come from any member, so the
 * peer is matched only when the request went to one host. */
void icmp_ping_reply6(const uint8_t *src, uint16_t id, uint16_t seq, uint16_t len, uint8_t hlim)
{
    ping_answer(0, src, id, seq, len, hlim);
}

/**
 * Whether the ping in flight `arg` is done: answered, or past its deadline.
 *
 * Both halves are here on purpose. `wq_wait_event` re-tests this after every
 * wake and waits again while it is false, with the deadline armed again, so a
 * predicate that read only `replied` would be woken at every tick once the
 * deadline had passed and would never return.
 */
static int ping_done(void *arg)
{
    struct icmp_pinger *p = arg;

    return p->replied || tick_count() >= p->deadline;
}

/**
 * Take the ping in flight `p` off the list. Called on every way out of
 * `ping_attempts` once it is on it.
 *
 * Under `net_lock` because `icmp_rx` walks the list from an interrupt, and
 * it clears the arming too: a deadline left armed wakes this process early
 * out of its next unrelated sleep, since `nanosleep` and `poll` read the same
 * `wakeup_tick`.
 */
static void ping_release(struct icmp_pinger *p)
{
    struct icmp_pinger **link;
    uint32_t flags = net_lock();

    for (link = &pingers; *link; link = &(*link)->next) {
        if (*link == p) {
            *link = p->next;
            break;
        }
    }
    net_unlock(flags);
    if (current_proc) {
        current_proc->wakeup_tick = 0;
    }
}

/**
 * The attempts of `ping_run`, each request built in `echo`, which holds
 * 8 + `size` bytes and is used only under `net_lock`.
 *
 * The caller's record is on the list for the whole call and comes off as
 * the call's last act on every path, so a killed pinger leaves nothing
 * behind: the signal makes the blocked process runnable, the wait breaks
 * with -EINTR, and the release runs before the system call returns. Each
 * request is built and sent under `net_lock`, and its deadline, a second's
 * ticks past the count, is never 0, `wakeup_tick`'s "not sleeping" value.
 * Whether a reply came is read from `replied`, since a wait that returns
 * does not say whether the reply or the deadline ended it.
 */
static int ping_attempts(uint32_t dst_ip, const uint8_t *dst6, int ifindex, uint32_t size,
                         uint8_t ttl, uint16_t *got, uint8_t *got_ttl, uint8_t *echo)
{
    struct icmp_pinger me;
    uint64_t start;
    uint32_t flags, i;
    int attempt;
    int rc = -ETIMEDOUT;

    k_memset(&me, 0, sizeof(me));
    wait_queue_init(&me.wq);
    me.v6 = dst6 != 0;
    me.peer_ip = dst_ip;
    if (dst6) {
        k_memcpy(me.peer6, dst6, sizeof(me.peer6));
    }

    flags = net_lock();
    me.id = icmp_echo_id();
    me.next = pingers;
    pingers = &me;
    net_unlock(flags);

    for (attempt = 0; attempt < ICMP_PING_ATTEMPTS; attempt++) {
        flags = net_lock();

        echo[0] = dst6 ? ICMP6_ECHO_REQUEST : ICMP_ECHO_REQUEST;
        echo[1] = 0;
        write16be(echo + 2, 0x0000);
        write16be(echo + 4, me.id);
        write16be(echo + 6, (uint16_t)attempt);
        for (i = 0; i < size; i++) {
            echo[8 + i] = (uint8_t)i;
        }
        if (!dst6) {
            write16be(echo + 2, ip_checksum(echo, (uint16_t)(8 + size)));
        }

        me.seq_wait = (uint16_t)attempt;
        me.replied = 0;
        start = rtc_ticks_now();
        me.deadline = start + ICMP_PING_WAIT;

        if (dst6) {
            int sent = icmp6_send(0, dst6, ifindex, echo, (uint16_t)(8 + size), ttl);

            if (sent < 0 && attempt == 0) {
                net_unlock(flags);
                ping_release(&me);
                return sent;
            }
        } else if (ip_tx(dst_ip, IP_PROTO_ICMP, echo, (uint16_t)(8 + size), ttl) == 0) {
            icmp_count_out(ICMP_ECHO_REQUEST);
        }

        net_unlock(flags);

        if (current_proc && current_proc->in_syscall) {
            int waited;

            current_proc->wakeup_tick = me.deadline;
            waited = wq_wait_event(&me.wq, ping_done, &me);
            current_proc->wakeup_tick = 0;
            if (waited < 0) {
                ping_release(&me);

                return waited;
            }
        } else {
            while (!me.replied && tick_count() < me.deadline) {
                idle_wait_irq_on();
            }
        }

        if (me.replied) {
            rc = (int)(rtc_ticks_now() - start);
            if (got) {
                *got = me.reply_len;
                *got_ttl = me.reply_ttl;
            }
            ping_release(&me);

            return rc;
        }
    }

    ping_release(&me);
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
