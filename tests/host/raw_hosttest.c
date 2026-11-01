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
 * the next is dropped and logged.
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

/** Record the queue a receive wakes and the kinds of change it names; no
 * process waits in this model. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    woken = wq;
    woken_kinds |= kinds;
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
    CHECK(raw_register(&sock, 1) == 0, "a socket registers for ICMP");
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
    printf("raw-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
