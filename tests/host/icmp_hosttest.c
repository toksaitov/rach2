/*
 * Exercise ping deadlines with timer interrupts still pending at each send.
 * host-deps: rach2/sys/network/ip/icmp.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "network/ip/icmp.h"
#include "network/ip/ip.h"
#include "network/core/byteorder.h"
#include "network/net.h"
#include "proc/process.h"
#include "proc/wait_queue.h"
#include "rtc.h"

extern int printf(const char *format, ...);

static process_t proc;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc}};
struct net_mib net_mib;
volatile uint32_t kernel_ticks;
static uint32_t now; /* current tick, including undelivered interrupts */
static uint32_t lag; /* ticks not yet counted by the interrupt handler */
static uint32_t sent_at;
static unsigned sends, waits;
static int reply;
static uint8_t echo[8];
static int failures;

/** Read a network-order halfword without alignment requirements. */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/** Read a network-order word without alignment requirements. */
uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/** Store a network-order halfword. */
void write16be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/** Store a network-order word. */
void write32be(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

#define CHECK(condition, detail)                           \
    do {                                                   \
        if (!(condition)) {                                \
            printf("icmp-hosttest: failed: %s\n", detail); \
            failures++;                                    \
        }                                                  \
    } while (0)

/** Read the tick independently of the delivered interrupt count. */
uint32_t rtc_ticks_now(void)
{
    return now;
}

/** No interrupt runs concurrently in this model. */
uint32_t net_lock(void)
{
    return 0;
}

/** Leave the model's network bracket. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** Discard the timeout diagnostic. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** No checksum is checked by the receiving path in this model. */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
    return 0;
}

/** Record the tick at which the request leaves and save its echo header. */
int ip_tx(uint32_t dst, uint8_t proto, const uint8_t *buf, uint16_t len, uint8_t ttl)
{
    unsigned i;

    CHECK(dst == 0x0A000263u && proto == IP_PROTO_ICMP && len == sizeof(echo) && ttl == 64,
          "send an echo request to the test peer");
    for (i = 0; i < sizeof(echo); i++) {
        echo[i] = buf[i];
    }
    sent_at = now;
    sends++;
    return 0;
}

/** No row sends an error or answers an echo request. */
int ip_tx_from(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *buf, uint16_t len,
               uint8_t ttl)
{
    (void)src;
    (void)dst;
    (void)proto;
    (void)buf;
    (void)len;
    (void)ttl;
    CHECK(0, "no unsolicited transmission");
    return 0;
}

/** No row tests local or broadcast addresses. */
int ip_is_local(uint32_t ip)
{
    (void)ip;
    return 0;
}

/** No row tests local or broadcast addresses. */
int netdev_is_broadcast(uint32_t ip)
{
    (void)ip;
    return 0;
}

/** No row receives a TCP error. */
void tcp_path_mtu(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq,
                  uint16_t mtu)
{
    (void)src;
    (void)dst;
    (void)sport;
    (void)dport;
    (void)seq;
    (void)mtu;
    CHECK(0, "no TCP error");
}

/** No row receives a UDP error. */
void udp_refused(uint16_t port, uint32_t ip, uint16_t peer)
{
    (void)port;
    (void)ip;
    (void)peer;
    CHECK(0, "no UDP error");
}

/** The model needs no queue links. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

/** The waiting callback delivers the reply synchronously. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Reach the armed deadline, or deliver a reply before the handler catches
 * up. Leave interrupts pending before the next attempt too. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    uint32_t deadline = proc.wakeup_tick;
    uint32_t expected = sent_at + 100u;

    (void)wq;
    waits++;
    if (!expected) {
        expected = 1u;
    }
    CHECK(deadline == expected, "deadline includes undelivered ticks");
    if (reply) {
        echo[0] = ICMP_ECHO_REPLY;
        icmp_rx(0x0A000263u, 0x0A00020Fu, 64, echo, sizeof(echo));
    } else {
        kernel_ticks = deadline - 1u;
        now = kernel_ticks;
        CHECK(!cond(arg), "the wait continues before its deadline");
        kernel_ticks = deadline;
        now = deadline + lag;
    }
    CHECK(cond(arg), "the reply or deadline ends the wait");
    return 0;
}

/** No row runs outside a system call. */
void idle_wait_irq_on(void)
{
    CHECK(0, "the pinger sleeps through the wait queue");
}

/** Exercise four attempts with no lag, with three undelivered ticks at
 * every send, and over the tick count's wrap; then an immediate reply with
 * the handler still behind the request's starting tick. */
int main(void)
{
    unsigned row;

    proc.in_syscall = 1;
    for (row = 0; row < 3; row++) {
        kernel_ticks = row == 2 ? 0xFFFFFF99u : 100u;
        lag = row == 0 ? 0u : 3u;
        now = kernel_ticks + lag;
        sends = waits = 0;
        CHECK(icmp_ping(0x0A000263u) == -110, "an unanswered ping times out");
        CHECK(sends == 4 && waits == 4, "four requests and four waits");
        CHECK(proc.wakeup_tick == 0, "the deadline is disarmed on return");
    }
    kernel_ticks = 100u;
    now = 103u;
    reply = 1;
    CHECK(icmp_ping(0x0A000263u) == 0, "an immediate reply has a zero round trip");
    CHECK(proc.wakeup_tick == 0, "a reply disarms the deadline");
    return failures != 0;
}
