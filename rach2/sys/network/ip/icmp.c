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
 * gives no other process any CPU at all for as long as the wait lasts, which on
 * a machine several people can log into is a freeze anybody could fire. The
 * boot shell keeps the poll, and correctly: it runs with the scheduler stopped
 * and no process to block, so there is nothing to yield to.
 *
 * The wait is bounded twice over, and both halves are necessary. The
 * process arms wakeup_tick so the timer makes it runnable, since a BLOCKED
 * process nothing wakes is one schedule() never picks again; and the deadline
 * is also in the wait predicate, since a woken sleeper whose condition is still
 * false re-enqueues with its timer already disarmed and would then sleep
 * forever. Both read the one deadline word, so a timer that has fired implies a
 * predicate that is true.
 *
 * There is one in-flight request for the whole machine, which is the raw-socket
 * non-goal made visible: with a real SOCK_RAW each caller would own its own
 * socket. The slot is claimed under net_lock and released on every exit path,
 * and a second caller is refused with -EBUSY, not queued, since a queue
 * needs a fairness rule to give a shell command a wait it cannot see.
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

/* Outgoing echo reply (sized for a full datagram) and request buffers. */
static uint8_t icmp_reply_buf[1500];
static uint8_t icmp_echo_buf[8];

/* Identifier we stamp on our own echo requests. */
#define ICMP_PING_ID 0x5243 /* 'RC' */

/* Per-attempt timeout (ticks) and number of attempts. */
#define ICMP_PING_WAIT     100 /* 1 s at 100 Hz */
#define ICMP_PING_ATTEMPTS 4

/* The one in-flight ping, matched against incoming replies. `ping_owner` is the
 * process that claimed the slot, or NULL for the boot shell, which has none. */
static volatile int ping_active = 0;
static volatile int ping_replied = 0;
static volatile uint16_t ping_seq_wait = 0;
static volatile uint32_t ping_peer_ip = 0;
static volatile uint32_t ping_deadline = 0;
static process_t *ping_owner = 0;
static uint16_t ping_seq = 0;

/* Where a sleeping pinger waits, and what icmp_rx wakes. */
static wait_queue_t ping_wq;
static int ping_wq_ready = 0;

/* Kernel tick counter, for the timeout. */
extern volatile uint32_t kernel_ticks;

/**
 * Handle one received ICMP datagram: answer echo requests, match echo replies.
 *
 * An echo request is reflected back to its source: the payload is copied
 * verbatim, the type is changed to reply, and the checksum is recomputed.
 * An echo reply is accepted only if it matches the single ping in flight by
 * source address, identifier, and sequence; a match sets `ping_replied`.
 *
 * @param src_ip source address of the datagram, host byte order.
 * @param len ICMP length; datagrams shorter than the 8-byte header are dropped.
 */
void icmp_rx(uint32_t src_ip, const uint8_t *data, uint16_t len)
{
    if (len < 8) {
        return;
    }

    if (data[0] == ICMP_ECHO_REQUEST) {
        if (len > 1500) {
            len = 1500;
        }
        k_memcpy(icmp_reply_buf, data, len);
        icmp_reply_buf[0] = ICMP_ECHO_REPLY;
        icmp_reply_buf[1] = 0;
        write16be(icmp_reply_buf + 2, 0x0000);
        write16be(icmp_reply_buf + 2, ip_checksum(icmp_reply_buf, len));
        ip_tx(src_ip, IP_PROTO_ICMP, icmp_reply_buf, len, IP_TTL_DEFAULT);
    } else if (data[0] == ICMP_ECHO_REPLY) {
        uint16_t id = read16be(data + 4);
        uint16_t seq = read16be(data + 6);
        if (ping_active && src_ip == ping_peer_ip && id == ICMP_PING_ID && seq == ping_seq_wait) {
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
 * wake and re-enqueues while it is false, and the timer that made the sleeper
 * runnable has already cleared its own arming, so a predicate that read only
 * `ping_replied` would block again with nothing left to wake it.
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
    ping_owner = 0;
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
 * cache.
 *
 * @return round-trip time in ticks, `-ETIMEDOUT` if every attempt expired,
 *         `-EBUSY` if another ping has the slot, or `-EINTR` on a signal.
 */
int icmp_ping(uint32_t dst_ip)
{
    uint32_t start, flags;
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
    ping_owner = current_proc;
    ping_replied = 0;
    net_unlock(flags);

    for (attempt = 0; attempt < ICMP_PING_ATTEMPTS; attempt++) {
        seq = ping_seq++;

        /* Build and send the request atomically; the wait below re-enables
         * interrupts so the reply can arrive. */
        flags = net_lock();

        icmp_echo_buf[0] = ICMP_ECHO_REQUEST;
        icmp_echo_buf[1] = 0;
        write16be(icmp_echo_buf + 2, 0x0000);
        write16be(icmp_echo_buf + 4, ICMP_PING_ID);
        write16be(icmp_echo_buf + 6, seq);
        write16be(icmp_echo_buf + 2, ip_checksum(icmp_echo_buf, 8));

        ping_peer_ip = dst_ip;
        ping_seq_wait = seq;
        ping_replied = 0;
        start = kernel_ticks;
        ping_deadline = start + ICMP_PING_WAIT;
        if (ping_deadline == 0) {
            /* 0 is `wakeup_tick`'s "not sleeping" sentinel, so a deadline that
             * lands there arms nothing and the sleeper is never made runnable
             * again. `sys_nanosleep` steps around the same tick. */
            ping_deadline = 1;
        }

        ip_tx(dst_ip, IP_PROTO_ICMP, icmp_echo_buf, 8, IP_TTL_DEFAULT);

        net_unlock(flags);

        if (current_proc && current_proc->in_syscall) {
            int waited;

            /* The scheduling half: the timer makes this process runnable at the
             * deadline. Without it nothing wakes a sleeper whose reply never
             * comes, and `schedule()` never picks it again. */
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

        /* A wait that returned says nothing about which half ended it, so the
         * answer is read from the state and not from the return. */
        if (ping_replied) {
            rc = (int)(kernel_ticks - start);
            ping_release();

            return rc;
        }
    }

    ping_release();
    klog(KLOG_INFO, "icmp", "ping: no reply before the timeout");

    return rc;
}
