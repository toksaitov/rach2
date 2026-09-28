/**
 * Queue datagrams for a raw socket with the kernel's raw.c.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 *
 * A datagram of the protocol a socket registered is queued, whole, with its
 * source, and wakes the socket's queue for data alone, so an edge-triggered
 * epoll registration for output, which a raw socket always has, is not told
 * of it. A datagram of another protocol is not queued and wakes nothing.
 */
#include "network/socket/raw.c"

extern int printf(const char *format, ...);

static wait_queue_t *woken;
static uint32_t woken_kinds;
static int failures;

#define CHECK(condition, detail)                          \
    do {                                                  \
        if (!(condition)) {                               \
            printf("raw-hosttest: failed: %s\n", detail); \
            failures++;                                   \
        }                                                 \
    } while (0)

/** No interrupt can enter a native row. */
uint32_t net_lock(void)
{
    return 0;
}

void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** No row fills a queue. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    CHECK(0, "no row fills a queue");
}

/** Record the queue a receive wakes and the kinds of change it names; no
 * process waits in this model. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    woken = wq;
    woken_kinds |= kinds;
}

int main(void)
{
    static struct socket sock;
    static uint8_t datagram[28];
    uint8_t first;

    datagram[0] = 0x45;
    datagram[9] = 1;
    datagram[12] = 10;
    datagram[15] = 2;
    CHECK(raw_register(&sock, 1) == 0, "a socket registers for ICMP");
    raw_rx(17, datagram, sizeof(datagram));
    CHECK(!raw_ready(&sock) && woken == NULL, "a datagram of another protocol wakes nothing");
    raw_rx(1, datagram, sizeof(datagram));
    CHECK(raw_ready(&sock) && raw_pending(&sock) == (int)sizeof(datagram),
          "a datagram of the registered protocol is queued whole");
    CHECK(raw_peek(&sock, 0, &first, 1) == 1 && first == 0x45,
          "the queued datagram is the one sent");
    raw_consume(&sock);
    CHECK(raw_last_src(&sock) == 0x0A000002u, "the datagram's source is kept");
    CHECK(woken == &sock.waiters && woken_kinds == WQ_DATA,
          "a datagram wakes its raw socket for data alone");
    raw_unregister(&sock);
    printf("raw-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
