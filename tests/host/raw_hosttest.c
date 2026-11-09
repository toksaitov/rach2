/**
 * Queue datagrams for a raw socket with the kernel's raw.c.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: rach2/sys/network/socket/address.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/network/socket/dgq.c
 *
 * A datagram of the protocol a socket registered is queued, whole, with its
 * source, on the socket's own queue, and wakes the socket for data alone, so
 * an edge-triggered epoll registration for output, which a raw socket always
 * has, is not told of it. A datagram of another protocol is not queued and
 * wakes nothing. A datagram past one frame, as a reassembled one is, is
 * queued whole too, and once the queue holds more than the socket's budget
 * the next is dropped and logged. An ICMP datagram socket takes only the
 * echo replies that carry its identifier and a good ICMP checksum, without
 * the IP header, and two such sockets hold different identifiers. Its
 * identifier is its port: a connect fixes it, and getsockname then reports
 * it, its requests carry it and its replies reach it; a bind gives it the
 * one asked for, which its requests then carry, unless another socket or a
 * ping of the kernel's holds that one; and a second bind is refused. A raw
 * socket takes an echo reply whatever its checksum.
 */
#include "network/socket/raw.c"

extern int printf(const char *format, ...);

static wait_queue_t *woken;  /* the queue wq_wake last named; NULL before */
static uint32_t woken_kinds; /* the WQ_ kinds of every wake, or-ed together */
static int failures;

#define CHECK(condition, detail)                          \
    do {                                                  \
        if (!(condition)) {                               \
            printf("raw-hosttest: failed: %s\n", detail); \
            failures++;                                   \
        }                                                 \
    } while (0)

/* The namespace of the datagram being delivered: always the first, the
 * model's only one. */
const struct netns *ip_ns;

/** The wall clock `sock_dgq_put` stamps a datagram with: the epoch, since
 * no test here reads a timestamp. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 0;
    *nanoseconds = 0;
}

/** Take the network lock: a no-op, since no interrupt runs in this model. */
uint32_t net_lock(void)
{
    return 0;
}

/** Release the network lock: a no-op. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

static int logged; /* the lines raw.c logged: one, for a full queue */

/** Count a line raw.c logs, which it does only when a queue is full. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    logged++;
}

/* The identifiers icmp_echo_id hands out, one past the last. */
static uint16_t next_id = 0x5243;

/* The identifier a ping of the kernel's holds in this model. */
#define KERNEL_PING_ID 0x7000

/** Whether a ping of the kernel's holds `id`: KERNEL_PING_ID alone. */
int icmp_ping_id_used(uint16_t id)
{
    return id == KERNEL_PING_ID;
}

/** The Internet checksum of the `len` bytes at `buf` (RFC 1071). */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    uint32_t sum = 0;
    uint16_t i;

    for (i = 0; i + 1 < len; i = (uint16_t)(i + 2)) {
        sum += (uint32_t)((buf[i] << 8) | buf[i + 1]);
    }
    if (len & 1) {
        sum += (uint32_t)buf[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

/** A new echo identifier, as the kernel's allocator gives one: none of
 * this model's sockets holds the next. */
uint16_t icmp_echo_id(void)
{
    return next_id++;
}

/** Record the queue a receive wakes and the kinds of change it names; no
 * process waits in this model. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    woken = wq;
    woken_kinds |= kinds;
}

/** Fill `d`, 28 bytes, as an IPv4 datagram from 10.0.0.2 holding an
 * ICMP message of `type` with the echo identifier `id` and a good
 * checksum. */
static void echo_datagram(uint8_t *d, uint8_t type, uint16_t id)
{
    uint16_t sum;

    uint32_t i;

    for (i = 0; i < 28; i++) {
        d[i] = 0;
    }
    d[0] = 0x45;
    d[9] = 1;
    d[12] = 10;
    d[15] = 2;
    d[20] = type;
    d[24] = (uint8_t)(id >> 8);
    d[25] = (uint8_t)id;
    d[27] = 7;
    sum = ip_checksum(d + 20, 8);
    d[22] = (uint8_t)(sum >> 8);
    d[23] = (uint8_t)sum;
}

/** The echo identifier in the ICMP message at `m`. */
static int id_of(const uint8_t *m)
{
    return (m[4] << 8) | m[5];
}

/** The rows of an ICMP datagram socket's identifier as its port: a
 * connected socket's, a bound one's, and the checksum of a reply. */
static void port_rows(void)
{
    static struct socket a, b, c, r;
    static uint8_t d[28], req[8];
    int ida;

    a.rcvbuf = b.rcvbuf = c.rcvbuf = r.rcvbuf = SOCK_RCVBUF_MIN;
    CHECK(raw_register(&a, 1, 1) == 0 && raw_register(&b, 1, 1) == 0 &&
              raw_register(&c, 1, 1) == 0 && raw_register(&r, 1, 0) == 0,
          "three ICMP datagram sockets and a raw one register");
    ida = raw_echo_id(&a);
    CHECK(raw_echo_port(&a) == 0, "an echo socket reports port 0 before a bind, send or connect");
    CHECK(raw_echo_port(&r) == -1, "a raw socket has no echo port");

    raw_echo_hold(&a); /* what connect does */
    CHECK(raw_echo_port(&a) == ida, "a connected echo socket reports its identifier as its port");
    req[0] = 8;
    req[2] = req[3] = 0xEE;
    CHECK(raw_echo_stamp(&a, req, (int)sizeof(req), 8) == 1 && id_of(req) == ida && req[2] == 0 &&
              req[3] == 0,
          "the connected socket's request carries its identifier, its checksum left to the send");
    echo_datagram(d, 0, (uint16_t)ida);
    raw_rx(1, d, sizeof(d));
    CHECK(a.rq.count == 1, "the connected socket receives its reply");
    CHECK(raw_echo_bind(&a, 0) == -EINVAL, "a connected socket's identifier is not bound again");

    CHECK(raw_echo_bind(&b, 0x1234) == 0 && raw_echo_port(&b) == 0x1234 &&
              raw_echo_id(&b) == 0x1234,
          "a bind gives the identifier asked for, and getsockname reports it");
    req[0] = 8;
    CHECK(raw_echo_stamp(&b, req, (int)sizeof(req), 8) == 1 && id_of(req) == 0x1234,
          "the requests of a bound socket carry the identifier bound");
    echo_datagram(d, 0, 0x1234);
    raw_rx(1, d, sizeof(d));
    CHECK(b.rq.count == 1 && a.rq.count == 1,
          "the reply to the identifier bound reaches its socket");
    CHECK(raw_echo_bind(&b, 0x1235) == -EINVAL, "a second bind is refused");
    CHECK(raw_echo_bind(&c, 0x1234) == -EADDRINUSE,
          "a bind to another socket's identifier is refused");
    CHECK(raw_echo_bind(&c, KERNEL_PING_ID) == -EADDRINUSE,
          "a bind to the identifier of a kernel's ping is refused");
    CHECK(raw_echo_port(&c) == 0, "a refused bind fixes nothing");
    CHECK(raw_echo_bind(&r, 0x2000) == -EINVAL, "a raw socket binds no identifier");

    echo_datagram(d, 0, 0x1234);
    d[23] ^= 1;
    raw_rx(1, d, sizeof(d));
    CHECK(b.rq.count == 1, "an echo reply with a bad ICMP checksum is dropped");
    CHECK(r.rq.count == 3, "a raw socket takes every echo reply, a bad checksum's too");
    CHECK(raw_echo_stamp(&c, req, 7, 8) == -1 && raw_echo_port(&c) == raw_echo_id(&c),
          "a send fixes the identifier even of a message refused");
    raw_unregister(&a);
    raw_unregister(&b);
    raw_unregister(&c);
    raw_unregister(&r);
}

/** The rows of two ICMP datagram sockets. */
static void echo_rows(void)
{
    static struct socket a, b;
    static uint8_t d[28], copy[8];
    int ida, idb;

    a.rcvbuf = b.rcvbuf = SOCK_RCVBUF_MIN;
    CHECK(raw_register(&a, 1, 1) == 0 && raw_register(&b, 1, 1) == 0,
          "two ICMP datagram sockets register");
    ida = raw_echo_id(&a);
    idb = raw_echo_id(&b);
    CHECK(ida >= 0 && idb >= 0 && ida != idb, "each holds an identifier of its own");
    CHECK(raw_echo_id_used((uint16_t)ida) && raw_echo_id_used((uint16_t)idb),
          "both identifiers are held");
    echo_datagram(d, 0, (uint16_t)ida);
    raw_rx(1, d, sizeof(d));
    CHECK(a.rq.count == 1 && a.rq.head->len == 8 && b.rq.count == 0,
          "an echo reply goes to the socket of its identifier alone");
    CHECK(sock_dgq_peek(&a.rq, 0, copy, (int)sizeof(copy)) == 8 && copy[0] == 0 && copy[7] == 7,
          "the reply is the ICMP message, without the IP header");
    echo_datagram(d, 8, (uint16_t)idb);
    raw_rx(1, d, sizeof(d));
    CHECK(b.rq.count == 0, "an echo request with a socket's identifier is not its");
    raw_unregister(&a);
    raw_unregister(&b);
    CHECK(!raw_echo_id_used((uint16_t)ida), "an identifier is free once its socket ends");
}

/** Run the rows; return 0 when every one passes and 1 otherwise. */
int main(void)
{
    static struct socket sock;
    static uint8_t datagram[28], copy[28], big[20000];
    uint32_t i;

    sock.rcvbuf = SOCK_RCVBUF_MIN;
    datagram[0] = 0x45;
    datagram[9] = 1;
    datagram[12] = 10;
    datagram[15] = 2;
    for (i = 20; i < sizeof(datagram); i++) {
        datagram[i] = (uint8_t)(0xA0 + i);
    }
    CHECK(raw_register(&sock, 1, 0) == 0, "a socket registers for ICMP");
    raw_rx(17, datagram, sizeof(datagram));
    CHECK(sock.rq.count == 0 && woken == NULL, "a datagram of another protocol wakes nothing");
    raw_rx(1, datagram, sizeof(datagram));
    CHECK(sock.rq.count == 1 && sock.rq.head->len == sizeof(datagram),
          "a datagram of the registered protocol is queued whole");
    CHECK(sock_dgq_peek(&sock.rq, 0, copy, (int)sizeof(copy)) == (int)sizeof(copy),
          "a peek gives the whole datagram");
    for (i = 0; i < sizeof(copy) && copy[i] == datagram[i]; i++) {
    }
    CHECK(i == sizeof(copy), "the queued datagram is the one sent, every byte of it");
    raw_consume(&sock);
    CHECK(raw_last_src(&sock) == 0x0A000002u, "the datagram's source is kept");
    CHECK(woken == &sock.waiters && woken_kinds == WQ_DATA,
          "a datagram wakes its raw socket for data alone");
    big[0] = 0x45;
    big[9] = 1;
    raw_rx(1, big, sizeof(big));
    CHECK(sock.rq.count == 1 && sock.rq.head->len == sizeof(big) && logged == 0,
          "a datagram past one frame is queued whole");
    raw_rx(1, datagram, sizeof(datagram));
    CHECK(sock.rq.count == 1 && logged == 1,
          "a datagram that finds the budget spent is dropped, and the drop logged");
    raw_unregister(&sock);
    CHECK(sock.rq.count == 0 && sock.rq.head == NULL, "unregistering frees what was queued");
    echo_rows();
    port_rows();
    printf("raw-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
