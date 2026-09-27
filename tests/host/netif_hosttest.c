/**
 * Exercise the socket address and option handlers with two interfaces, the
 * creation flags of socket and accept4, an accept the heap has no room to
 * give a descriptor, a recvfrom given an address and no length pointer, and
 * the descriptor accept reserves while it copies the peer's address.
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
static int32_t sys_accept4(isr_params_t *params);
static int32_t sys_bind(isr_params_t *params);
static int32_t sys_connect(isr_params_t *params);
static int32_t sys_getpeername(isr_params_t *params);
static int32_t sys_getsockname(isr_params_t *params);
static int32_t sys_getsockopt(isr_params_t *params);
static int32_t sys_listen(isr_params_t *params);
static int32_t sys_recvfrom(isr_params_t *params);
static int32_t sys_recvmmsg(isr_params_t *params);
static int32_t sys_recvmsg(isr_params_t *params);
static int32_t sys_sendmmsg(isr_params_t *params);
static int32_t sys_sendmsg(isr_params_t *params);
static int32_t sys_sendto(isr_params_t *params);
static int32_t sys_setsockopt(isr_params_t *params);
static int32_t sys_shutdown(isr_params_t *params);
static int32_t sys_socket(isr_params_t *params);
static int32_t sys_socketpair(isr_params_t *params);
#include "syscall_net.c"
#include "network/socket/packet.c"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);
extern int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

static process_t proc;
static fd_table_t files;
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
static int alloc_to = -1;
static int takes;
static struct socket made;
static fd_entry_t table[8];
static struct open_file made_description;
static void (*copy_sleep)(void);
static int sibling_found = -1;

#define CHECK(condition, detail)                          \
    do {                                                  \
        if (!(condition)) {                               \
            dprintf(2, "netif-test: fail: %s\n", detail); \
            failures++;                                   \
        }                                                 \
    } while (0)

/** Copy a simulated user range, refusing null and out-of-arena pointers. A
 * row may arm `copy_sleep`, the call another thread makes while the copy
 * sleeps in a page fault, which runs once before the copy. */
int copy_from_user(void *dst, const void *src, uint32_t n)
{
    unsigned long at = (unsigned long)src;
    void (*sleep)(void) = copy_sleep;

    copy_sleep = NULL;
    if (sleep) {
        sleep();
    }
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

/** The descriptor each row works on, 3, or one of the table the creating
 * rows fill; a slot that is free or only reserved is no descriptor. */
fd_entry_t *fd_get(fd_table_t *fds, int fd)
{
    if (fd == 3) {
        return &entry;
    }
    return fds == &files && fd >= 0 && fd < 8 && table[fd].type != FD_TYPE_NONE ? &table[fd] : NULL;
}

/** Descriptor 4 for the socket rows that create one, or the refusal
 * `alloc_to` has for the row that is refused one; an `alloc_to` of -1 is
 * for the rows that may not allocate, which invalid accepts are. */
int fd_alloc(fd_table_t *fds)
{
    (void)fds;
    CHECK(alloc_to != -1, "invalid accept must not allocate a descriptor");
    return alloc_to;
}

/** The one socket record the creating rows make. */
struct socket *socket_alloc(int type)
{
    k_memset(&made, 0, sizeof(made));
    made.type = type;
    return &made;
}

/** No row makes a raw ICMP socket. */
int raw_register(struct socket *s, uint8_t proto)
{
    (void)s;
    (void)proto;
    CHECK(0, "no row registers a raw socket");
    return -ENOBUFS;
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

/** The user range a receive checks: inside the simulated user memory. */
int user_buffer_ok(uint32_t addr, uint32_t len, int write)
{
    (void)write;
    return addr && addr <= sizeof(user) && len <= sizeof(user) - addr;
}

/** No row's socket is the query descriptor. */
int sock_is_query(const struct socket *s)
{
    (void)s;
    return 0;
}

/** Count the receives that take a datagram or a stream's bytes, each of
 * which finds one byte. */
int socket_recv(struct socket *s, uint8_t *buf, int len, uint32_t *src_ip, uint16_t *src_port)
{
    (void)s;
    (void)buf;
    (void)len;
    *src_ip = net_ip_addr;
    *src_port = 7;
    takes++;
    return 1;
}

int socket_try_recv(struct socket *s, uint8_t *buf, int len, uint32_t *src_ip, uint16_t *src_port)
{
    return socket_recv(s, buf, len, src_ip, src_port);
}

int socket_tcp_recv(struct socket *s, uint8_t *buf, int len)
{
    (void)s;
    (void)buf;
    (void)len;
    takes++;
    return 1;
}

int socket_tcp_try_recv(struct socket *s, uint8_t *buf, int len)
{
    return socket_tcp_recv(s, buf, len);
}

/** Required by the accept allocation failure path. */
void socket_unref(struct socket *s)
{
    (void)s;
    CHECK(0, "invalid accept must not release a socket");
}

/** A wait for a condition already true returns at once, as the real one
 * does; any other returns EINTR, so a missing accept check fails without
 * hanging. */
int wq_wait_event(wait_queue_t *q, int (*ready)(void *), void *arg)
{
    (void)q;
    if (ready(arg)) {
        return 0;
    }
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
        CHECK(do_accept(3, 0, 0, 0) == -EINVAL, "datagram accept returns einval");
        sock.type = SOCK_STREAM;
        CHECK(do_accept(3, 0, 0, 0) == -EINVAL, "nonlistener accept returns einval");
        sock.type = SOCK_RAW;
        CHECK(do_accept(3, 0, 0, 0) == -EOPNOTSUPP, "raw accept remains unsupported");
    }
    CHECK(do_accept(3, 0, 0, 0x1) == -EINVAL,
          "accept4 refuses a flag other than SOCK_CLOEXEC and SOCK_NONBLOCK");
    CHECK(waits == 0, "invalid accept never waits");
    sock.type = SOCK_STREAM;
    sock.listening = 1;
    CHECK(do_accept(3, 0, 0, 0) == -EAGAIN, "empty nonblocking listener still returns eagain");
    CHECK(do_accept(3, 0, 0, SOCK_CLOEXEC | SOCK_NONBLOCK) == -EAGAIN,
          "accept4 takes SOCK_CLOEXEC and SOCK_NONBLOCK");
}

/** The creation flags in a socket's type: taken onto the new descriptor, and
 * any other bit above the type's low four refused before the family. */
static void socket_flags(void)
{
    proc.fds = &files;
    files.entries = table;
    table[4].ofd = &made_description;
    CHECK(socket_with_flags(AF_INET, SOCK_STREAM | 0x10, 0) == -EINVAL,
          "a type bit other than SOCK_CLOEXEC and SOCK_NONBLOCK is refused");
    CHECK(socket_with_flags(1, SOCK_STREAM | 0x100000, 0) == -EINVAL,
          "a bad type bit is refused before the family");
    CHECK(socket_with_flags(1, SOCK_STREAM | SOCK_CLOEXEC, 0) == -EAFNOSUPPORT,
          "the creation flags leave the family's check as it was");
    alloc_to = 4;
    CHECK(socket_with_flags(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0) == 4 &&
              made.type == SOCK_STREAM,
          "a stream socket is made with both creation flags");
    CHECK((table[4].flags & FD_FLAG_CLOEXEC) && (made_description.status_flags & OFD_NONBLOCK),
          "SOCK_CLOEXEC and SOCK_NONBLOCK reach the descriptor");
    made_description.status_flags = 0;
    CHECK(socket_with_flags(AF_INET, SOCK_DGRAM, 0) == 4 && made.type == SOCK_DGRAM,
          "a datagram socket is made without creation flags");
    CHECK(!(table[4].flags & FD_FLAG_CLOEXEC) && !(made_description.status_flags & OFD_NONBLOCK),
          "a socket made without the flags has neither");
    alloc_to = -1;
}

/** A queued connection accepted with both creation flags gets both, and one
 * accepted with none gets neither. */
static void accept_flags(void)
{
    static struct socket conn[2];
    int i;

    proc.fds = &files;
    files.entries = table;
    table[4].ofd = &made_description;
    sock.type = SOCK_STREAM;
    sock.listening = 1;
    alloc_to = 4;
    for (i = 0; i < 2; i++) {
        sock.accept_queue[sock.accept_tail] = &conn[i];
        sock.accept_tail = (sock.accept_tail + 1) & (SOCK_ACCEPT_BACKLOG - 1);
    }
    made_description.status_flags = 0;
    CHECK(do_accept(3, 0, 0, SOCK_CLOEXEC | SOCK_NONBLOCK) == 4 && table[4].sock == &conn[0],
          "accept4 with both flags takes the first queued connection");
    CHECK((table[4].flags & FD_FLAG_CLOEXEC) && (made_description.status_flags & OFD_NONBLOCK),
          "a connection accepted with both flags has both");
    made_description.status_flags = 0;
    CHECK(do_accept(3, 0, 0, 0) == 4 && table[4].sock == &conn[1],
          "accept takes the second queued connection");
    CHECK(!(table[4].flags & FD_FLAG_CLOEXEC) && !(made_description.status_flags & OFD_NONBLOCK),
          "a connection accepted with no flags has neither");
    alloc_to = -1;
}

/** An accept whose descriptor the heap has no room for returns ENOMEM, as
 * the allocator answered, and leaves the connection queued. */
static void accept_refused(void)
{
    static struct socket conn;
    int head = sock.accept_head;

    sock.type = SOCK_STREAM;
    sock.listening = 1;
    sock.accept_queue[sock.accept_tail] = &conn;
    sock.accept_tail = (sock.accept_tail + 1) & (SOCK_ACCEPT_BACKLOG - 1);
    alloc_to = -ENOMEM;
    CHECK(do_accept(3, 0, 0, 0) == -ENOMEM && sock.accept_head == head,
          "an accept the heap has no room for is ENOMEM and leaves the connection queued");
    alloc_to = -1;
    sock.accept_head = sock.accept_tail;
}

/** A recvfrom given an address and no length pointer is EFAULT, as on
 * Linux, and takes nothing, on a stream as on a datagram socket, where Linux
 * takes the message first; one given a length pointer takes the datagram and
 * reports its sender. */
static void recvfrom_no_length(void)
{
    int type;

    sock.family = AF_INET;
    for (type = SOCK_STREAM; type <= SOCK_DGRAM; type++) {
        sock.type = type;
        takes = 0;
        CHECK(do_recvfrom(3, 64, 8, 0, 32, 0) == -EFAULT && takes == 0,
              "a recvfrom with an address and no length is EFAULT before it takes anything");
    }
    takes = 0;
    SC_WLE32(user + 8, 16);
    CHECK(do_recvfrom(3, 64, 8, 0, 32, 8) == 1 && takes == 1 && SC_LE32(user + 8) == 16 &&
              SC_BE16(user + 34) == 7,
          "a recvfrom with a length pointer takes the datagram and reports its sender");
}

/** Another thread's close of descriptor 4 while a copy sleeps: whether it
 * found a descriptor there, which it closes. */
static void close_four(void)
{
    fd_entry_t *e = fd_get(&files, 4);

    sibling_found = e != NULL;
    if (e) {
        e->type = FD_TYPE_NONE;
    }
}

/** The descriptor accept makes is reserved while the peer's address is
 * copied out, which can sleep: another thread's close of its number then
 * finds nothing, and the connection is published after the copy. */
static void accept_reserved(void)
{
    static struct socket conn;

    proc.fds = &files;
    files.entries = table;
    k_memset(&table[4], 0, sizeof(table[4]));
    table[4].ofd = &made_description;
    sock.type = SOCK_STREAM;
    sock.listening = 1;
    alloc_to = 4;
    sock.accept_queue[sock.accept_tail] = &conn;
    sock.accept_tail = (sock.accept_tail + 1) & (SOCK_ACCEPT_BACKLOG - 1);
    SC_WLE32(user + 8, 16);
    copy_sleep = close_four;
    CHECK(do_accept(3, 32, 8, 0) == 4, "an accept with an address takes the queued connection");
    CHECK(sibling_found == 0, "another thread's close of the number accept reserved finds nothing "
                              "while the copy of the peer's address sleeps");
    CHECK(table[4].type == FD_TYPE_SOCKET && table[4].sock == &conn && SC_LE32(user + 8) == 16,
          "accept publishes the connection once the peer's address is copied");
    alloc_to = -1;
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
    socket_flags();
    accept_flags();
    accept_refused();
    recvfrom_no_length();
    accept_reserved();
    if (!failures) {
        printf("netif-test: pass: socket interface contracts\n");
    }
    return failures ? 1 : 0;
}
