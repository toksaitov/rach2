/*
 * Exercise ping deadlines with timer interrupts still pending at each send,
 * and the check of a fragmentation-needed report against its connection.
 * host-deps: rach2/sys/network/ip/icmp.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/network/socket/address.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c tests/host/compat/kheap_host.c
 */
#include "network/ip/icmp.h"
#include "network/ip/ip.h"
#include "network/core/byteorder.h"
#include "network/net.h"
#include "network/tcp/tcp.h"
#include "lib/kstring.h"
#include "proc/process.h"
#include "proc/wait_queue.h"
#include "rtc.h"
#include "timer.h"

extern int printf(const char *format, ...);

static process_t proc;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc}};
struct net_mib net_mib;
volatile uint64_t kernel_ticks;

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}
static uint64_t now; /* current tick, including undelivered interrupts */
static uint32_t lag; /* ticks not yet counted by the interrupt handler */
static uint64_t sent_at;
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
uint64_t rtc_ticks_now(void)
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

/** No row pings over IPv6. */
int icmp6_send(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
               uint8_t hlim)
{
    (void)src;
    (void)dst;
    (void)ifindex;
    (void)msg;
    (void)len;
    (void)hlim;
    CHECK(0, "no IPv6 echo request");
    return 0;
}

/** No row tests local or broadcast addresses. */
int ip_is_local(uint32_t ip)
{
    (void)ip;
    return 0;
}

/* The model's address, and the one connection the reports name: from
 * HOST:HOST_PORT to PEER:PEER_PORT, with bytes FLIGHT_UNA up to FLIGHT_NXT
 * sent and unacknowledged. */
#define HOST       0x0A00020Fu
#define PEER       0x0A000263u
#define HOST_PORT  40000u
#define PEER_PORT  80u
#define FLIGHT_UNA 1000u
#define FLIGHT_NXT 3000u

/* The path MTU the last report set, 0 for none, and the reports TCP took. */
static uint32_t pmtu;
static unsigned tcp_reports;

/** Only HOST is this machine's. */
int netdev_owns(uint32_t ip)
{
    return ip == HOST;
}

/** Record the path MTU a report sets for PEER, the only destination. */
void ip_pmtu_lower(uint32_t dst, uint32_t mtu)
{
    CHECK(dst == PEER, "only the peer's path MTU falls");
    pmtu = mtu;
}

/** No row tests local or broadcast addresses. */
int netdev_is_broadcast(uint32_t ip)
{
    (void)ip;
    return 0;
}

/** Whether `seq` is in flight on the model's one connection, as tcp.c
 * judges it. */
int tcp_in_flight(net_addr_t local_ip, uint16_t local_port, net_addr_t remote_ip,
                  uint16_t remote_port, uint32_t seq)
{
    return net_addr_equal(local_ip, net_addr4(HOST)) && local_port == HOST_PORT &&
           net_addr_equal(remote_ip, net_addr4(PEER)) && remote_port == PEER_PORT &&
           (int32_t)(seq - FLIGHT_UNA) >= 0 && (int32_t)(seq - FLIGHT_NXT) < 0;
}

/** Count a report TCP takes, which must be one in flight. */
void tcp_path_mtu(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq,
                  uint16_t mtu)
{
    CHECK(tcp_in_flight(net_addr4(src), sport, net_addr4(dst), dport, seq),
          "TCP takes only a report in flight");
    CHECK(mtu == pmtu, "TCP takes the MTU the path has");
    tcp_reports++;
}

/**
 * Give icmp_rx a fragmentation-needed report of `mtu` that quotes a
 * datagram of protocol `proto` from HOST:`sport` to PEER:PEER_PORT, whose
 * first eight bytes after the header are the ports and the sequence `seq`;
 * `quote` is how many of them the report carries.
 */
static void frag_needed(uint8_t proto, uint16_t sport, uint32_t seq, uint16_t mtu, uint16_t quote)
{
    uint8_t m[8 + 20 + 8];

    k_memset(m, 0, sizeof(m));
    m[0] = ICMP_UNREACHABLE;
    m[1] = ICMP_FRAG_NEEDED;
    write16be(m + 6, mtu);
    m[8] = 0x45;
    m[8 + 9] = proto;
    write32be(m + 8 + 12, HOST);
    write32be(m + 8 + 16, PEER);
    write16be(m + 28, sport);
    write16be(m + 30, PEER_PORT);
    write32be(m + 32, seq);
    icmp_rx(PEER, HOST, 64, m, (uint16_t)(28 + quote));
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
    uint64_t deadline = proc.wakeup_tick;
    uint64_t expected = sent_at + TICK_HZ;

    (void)wq;
    waits++;
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
 * every send, and across 2^32 ticks; then an immediate reply with the
 * handler still behind the request's starting tick. */
int main(void)
{
    unsigned row;

    proc.in_syscall = 1;
    for (row = 0; row < 3; row++) {
        kernel_ticks = row == 2 ? 0xFFFFF99Aull : 100u;
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

    frag_needed(IP_PROTO_TCP, HOST_PORT, FLIGHT_NXT + 5000u, 1280, 8);
    CHECK(pmtu == 0 && tcp_reports == 0, "a sequence not in flight leaves the path MTU");
    frag_needed(IP_PROTO_TCP, HOST_PORT, FLIGHT_UNA - 1u, 1280, 8);
    CHECK(pmtu == 0 && tcp_reports == 0, "an acknowledged sequence leaves the path MTU");
    frag_needed(IP_PROTO_TCP, HOST_PORT + 1u, FLIGHT_UNA, 1280, 8);
    CHECK(pmtu == 0 && tcp_reports == 0, "another connection's report leaves the path MTU");
    frag_needed(IP_PROTO_TCP, HOST_PORT, FLIGHT_UNA, 1280, 4);
    CHECK(pmtu == 0 && tcp_reports == 0, "a quote without the sequence leaves the path MTU");
    frag_needed(IP_PROTO_TCP, HOST_PORT, FLIGHT_UNA + 1u, 1280, 8);
    CHECK(pmtu == 1280 && tcp_reports == 1, "a sequence in flight lowers the path MTU");
    frag_needed(IP_PROTO_UDP, 5353u, 0, 1000, 8);
    CHECK(pmtu == 1000 && tcp_reports == 1, "a UDP report lowers the path MTU, TCP untold");
    return failures != 0;
}
