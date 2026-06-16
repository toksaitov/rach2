/*
 * icmp.c - Internet Control Message Protocol (echo only).
 *
 * Echo format (big-endian): type(1) code(1) checksum(2) id(2) seq(2) data(N).
 *
 * Receiving an echo request: copy it back, flip the type to reply, recompute
 * the checksum, and send. Receiving an echo reply: if it matches the ping in
 * flight (source, id, and sequence), mark it answered.
 *
 * icmp_ping sends a request and polls for the reply with sti/hlt. It must run
 * in a context the timer will not preempt away (a syscall, where in_syscall is
 * set, or the boot shell where nothing else is runnable); the timer still
 * fires, so kernel_ticks advances and the timeout works. The first request on
 * a cold ARP cache is dropped, so we retry a few times: the early attempt
 * warms the cache and a later one gets through.
 */

#include "icmp.h"
#include "ip.h"
#include "../net.h"
#include "../core/byteorder.h"
#include "../../serial.h"
#include "../../lib/kstring.h"

/* Outgoing echo reply (sized for a full datagram) and request buffers. */
static uint8_t icmp_reply_buf[1500];
static uint8_t icmp_echo_buf[8];

/* Identifier we stamp on our own echo requests. */
#define ICMP_PING_ID    0x5243   /* 'RC' */

/* Per-attempt timeout (ticks) and number of attempts. */
#define ICMP_PING_WAIT      100  /* 1 s at 100 Hz */
#define ICMP_PING_ATTEMPTS  4

/* State for the single in-flight ping, matched against incoming replies. */
static volatile int      ping_active   = 0;
static volatile int      ping_replied  = 0;
static volatile uint16_t ping_seq_wait = 0;
static volatile uint32_t ping_peer_ip  = 0;
static uint16_t          ping_seq      = 0;

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
        ip_tx(src_ip, IP_PROTO_ICMP, icmp_reply_buf, len);
    } else if (data[0] == ICMP_ECHO_REPLY) {
        uint16_t id  = read16be(data + 4);
        uint16_t seq = read16be(data + 6);
        if (ping_active && src_ip == ping_peer_ip &&
            id == ICMP_PING_ID && seq == ping_seq_wait) {
            ping_replied = 1;
        }
    }
}

/**
 * Send an ICMP echo request to `dst_ip` and wait for the matching reply.
 *
 * Caller must run where the timer will not preempt it away (a syscall, or the
 * boot shell). The first request on a cold ARP cache is dropped, so up to
 * `ICMP_PING_ATTEMPTS` are sent; an early attempt warms the cache. Each attempt
 * polls with sti/hlt for up to `ICMP_PING_WAIT` ticks.
 *
 * @return round-trip time in ticks, or -1 if every attempt timed out.
 */
int icmp_ping(uint32_t dst_ip)
{
    uint32_t start, flags;
    uint16_t seq;
    int attempt;

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

        ping_peer_ip  = dst_ip;
        ping_seq_wait = seq;
        ping_replied  = 0;
        ping_active   = 1;

        ip_tx(dst_ip, IP_PROTO_ICMP, icmp_echo_buf, 8);

        net_unlock(flags);

        start = kernel_ticks;
        while (!ping_replied &&
               (int32_t)(kernel_ticks - start) < ICMP_PING_WAIT) {
            __asm__ __volatile__("sti; hlt");
        }

        if (ping_replied) {
            ping_active = 0;
            return (int)(kernel_ticks - start);
        }
    }

    ping_active = 0;
    serial_printf("ICMP: ping timeout\n");
    return -1;
}
