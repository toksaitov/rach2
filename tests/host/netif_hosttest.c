/**
 * Exercise the socket address and option handlers with two interfaces.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-cflags: -Wno-int-to-pointer-cast
 *
 * User pointers are offsets in an arena, preserving the 32-bit syscall ABI
 * on a 64-bit host. Unused syscall entry points are private to this test so
 * the compiler discards handlers whose dependencies are not modelled here.
 */
#include "syscall.h"
#include "network/socket/socket.h"
static int32_t sys_socketcall(isr_params_t *params);
static int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port,
                          const uint8_t *buf, uint16_t len);
#include "syscall_net.c"
#include "network/socket/packet.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);
extern int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

static process_t proc;
process_t *current_proc = &proc;
uint32_t net_ip_addr = 0x0a00020fu;
uint8_t net_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
static struct netdev cards[2];
static struct socket sock;
static struct tcb control;
static fd_entry_t entry;
static struct open_file description;
static uint8_t user[128];
static int failures;
static int waits;

#define CHECK(condition, detail)                          \
    do {                                                  \
        if (!(condition)) {                               \
            dprintf(2, "netif-test: fail: %s\n", detail); \
            failures++;                                   \
        }                                                 \
    } while (0)

/** Copy a simulated user range, refusing null and out-of-arena pointers. */
int copy_from_user(void *dst, const void *src, uint32_t n)
{
    unsigned long at = (unsigned long)src;

    if (!at || at > sizeof(user) || n > sizeof(user) - at) {
        return -EFAULT;
    }
    k_memcpy(dst, user + at, n);
    return 0;
}

/** Copy back into the same simulated user address space. */
int copy_to_user(void *dst, const void *src, uint32_t n)
{
    unsigned long at = (unsigned long)dst;

    if (!at || at > sizeof(user) || n > sizeof(user) - at) {
        return -EFAULT;
    }
    k_memcpy(user + at, src, n);
    return 0;
}

/** Return the sole descriptor used by each row. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    return fd == 3 ? &entry : NULL;
}

/** No row dequeues a connection. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    CHECK(0, "invalid accept must not allocate a descriptor");
    return -1;
}

/** Required by the accept error path, which these rows must not reach. */
int fd_free(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    CHECK(0, "invalid accept must not free a descriptor");
    return 0;
}

/** Required by the accept error path. */
void fd_release_backing(fd_entry_t *e)
{
    (void)e;
    CHECK(0, "invalid accept must not release a descriptor");
}

/** Required by the accept allocation failure path. */
void socket_unref(struct socket *s)
{
    (void)s;
    CHECK(0, "invalid accept must not release a socket");
}

/** A wait returns EINTR so a missing accept check fails without hanging. */
int wq_wait_event(wait_queue_t *q, int (*ready)(void *), void *arg)
{
    (void)q;
    (void)ready;
    (void)arg;
    waits++;
    return -EINTR;
}

/** Packet receive has no waiters in this model. */
void wq_wake_all(wait_queue_t *q)
{
    (void)q;
}

/** No interrupt can enter a native row. */
uint32_t net_lock(void)
{
    return 0;
}

/** Restore the native row's unchanged interrupt state. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** The model has two independently named interfaces. */
int netdev_count(void)
{
    return 2;
}

/** Match the registry's zero-based lookup. */
struct netdev *netdev_at(int n)
{
    return n >= 0 && n < 2 ? &cards[n] : NULL;
}

/** Report the first interface for packet transmit's default path. */
struct netdev *netdev_first(void)
{
    return &cards[0];
}

/** Resolve only complete interface names. */
struct netdev *netdev_by_name(const char *name)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (k_strlen(name) == 4 && memcmp(name, cards[i].name, 5) == 0) {
            return &cards[i];
        }
    }
    return NULL;
}

/** These rows do not transmit packets. */
int ethernet_tx(struct netdev *nd, const uint8_t *dst, uint16_t type, const uint8_t *data,
                uint16_t len)
{
    (void)nd;
    (void)dst;
    (void)type;
    (void)data;
    (void)len;
    CHECK(0, "address query must not transmit");
    return NETDEV_TX_OK;
}

/** No test frame fills a packet queue. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Set and read a device name, including refusal and clearing. */
static void device_option(void)
{
    k_memcpy(user + 16, "eth1", 5);
    CHECK(do_setsockopt(3, SOL_SOCKET, SO_BINDTODEVICE, 16, 5) == 0, "set eth1");
    CHECK(sock.bound_device == 2, "device index is two");
    SC_WLE32(user + 8, 16);
    CHECK(do_getsockopt(3, SOL_SOCKET, SO_BINDTODEVICE, 32, 8) == 0, "read eth1");
    CHECK(SC_LE32(user + 8) == 5 && memcmp(user + 32, "eth1", 5) == 0, "device name is eth1");
    k_memcpy(user + 16, "eth9", 5);
    CHECK(do_setsockopt(3, SOL_SOCKET, SO_BINDTODEVICE, 16, 5) == -ENODEV,
          "unknown device refused");
    CHECK(sock.bound_device == 2, "failed device bind preserves index");
    user[16] = 0;
    CHECK(do_setsockopt(3, SOL_SOCKET, SO_BINDTODEVICE, 16, 1) == 0, "clear device");
    SC_WLE32(user + 8, 16);
    CHECK(do_getsockopt(3, SOL_SOCKET, SO_BINDTODEVICE, 32, 8) == 0, "read cleared device");
    CHECK(!sock.bound_device && SC_LE32(user + 8) == 1 && !user[32], "device is clear");
}

/** Report packet bindings and preserve sockaddr truncation and length rules. */
static void packet_names(void)
{
    int index;

    sock.family = AF_PACKET;
    sock.protocol = ETH_TYPE_ARP;
    for (index = 2; index >= 0; index--) {
        CHECK(packet_bind(&sock, index) == 0, "packet bind");
        k_memset(user + 32, 0xa5, 21);
        SC_WLE32(user + 8, 20);
        CHECK(do_getsockname(3, 32, 8) == 0, "packet getsockname");
        CHECK(SC_LE32(user + 36) == (uint32_t)index, "packet interface index");
        CHECK(SC_LE32(user + 8) == 20 && user[52] == 0xa5, "packet address length");
        CHECK(user[32] == AF_PACKET && SC_BE16(user + 34) == ETH_TYPE_ARP,
              "packet family and protocol");
        if (index) {
            CHECK(user[40] == ARPHRD_ETHER && user[43] == 6 &&
                      memcmp(user + 44, cards[index - 1].mac, 6) == 0,
                  "packet interface mac");
        } else {
            CHECK(user[40] == 0 && user[43] == 0 && user[44] == 0,
                  "all-interface binding has no mac");
        }
    }
    CHECK(packet_bind(&sock, 2) == 0, "packet rebind");
    k_memset(user + 32, 0xa5, 20);
    SC_WLE32(user + 8, 7);
    CHECK(do_getsockname(3, 32, 8) == 0 && SC_LE32(user + 8) == 20 && user[39] == 0xa5,
          "packet truncated address");
    packet_unbind(&sock);
    SC_WLE32(user + 8, 20);
    CHECK(do_getsockname(3, 32, 8) == 0 && SC_LE32(user + 36) == 0, "unbound packet index is zero");
}

/** A TCP endpoint reports its saved local address, including passive opens. */
static void inet_names(void)
{
    sock.family = AF_INET;
    sock.type = SOCK_STREAM;
    sock.local_port = 7400;
    sock.tcb = &control;
    control.local_ip = cards[1].ip;
    SC_WLE32(user + 8, 16);
    CHECK(do_getsockname(3, 32, 8) == 0, "tcp getsockname");
    CHECK(SC_BE32(user + 36) == cards[1].ip, "tcp local address");
    CHECK(SC_BE16(user + 34) == 7400 && SC_LE32(user + 8) == 16, "tcp port and length");
    sock.tcb = NULL;
    SC_WLE32(user + 8, 16);
    CHECK(do_getsockname(3, 32, 8) == 0 && SC_BE32(user + 36) == net_ip_addr,
          "socket without control block keeps primary address");
}

/** Reject invalid accept calls before either waiting or checking nonblock. */
static void invalid_accept(void)
{
    int nonblock;

    for (nonblock = 0; nonblock <= 1; nonblock++) {
        description.status_flags = nonblock ? OFD_NONBLOCK : 0;
        sock.type = SOCK_DGRAM;
        CHECK(do_accept(3, 0, 0) == -EINVAL, "datagram accept returns einval");
        sock.type = SOCK_STREAM;
        CHECK(do_accept(3, 0, 0) == -EINVAL, "nonlistener accept returns einval");
        sock.type = SOCK_RAW;
        CHECK(do_accept(3, 0, 0) == -EOPNOTSUPP, "raw accept remains unsupported");
    }
    CHECK(waits == 0, "invalid accept never waits");
    sock.type = SOCK_STREAM;
    sock.listening = 1;
    CHECK(do_accept(3, 0, 0) == -EAGAIN, "empty nonblocking listener still returns eagain");
}

/** Run the four syscall contracts against distinct addresses and MACs. */
int main(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        cards[i].index = i + 1;
        k_memcpy(cards[i].name, i ? "eth1" : "eth0", 5);
        k_memcpy(cards[i].mac, net_mac, 6);
        cards[i].mac[5] += (uint8_t)i;
        cards[i].ip = i ? 0x0a000305u : net_ip_addr;
    }
    entry.type = FD_TYPE_SOCKET;
    entry.sock = &sock;
    entry.ofd = &description;
    device_option();
    packet_names();
    inet_names();
    invalid_accept();
    if (!failures) {
        printf("netif-test: pass: socket interface contracts\n");
    }
    return failures ? 1 : 0;
}
