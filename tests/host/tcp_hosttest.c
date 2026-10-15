/*
 * tcp_hosttest.c: the TCP state machine, driven segment by segment.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/network/tcp/rcvwnd.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/pressure_host.c
 *
 * A guest can make this stack open a connection and carry bytes, and the
 * network suites do. What it cannot make happen on demand is the rest: a
 * segment that arrives twice, a segment that arrives out of order, a hole in
 * the sequence space, a peer that stops acknowledging until the retransmission
 * gives up, a window that closes and reopens, a segment for a port nobody
 * listens on, and a driver that fails to send. Those are the cases
 * here, driven directly against `tcp.c` with the layer under it replaced by a
 * recorder that keeps every segment `ip_tx` would have sent, and the layer
 * above it by one that records every callback.
 *
 * The clock is a variable, so retransmission timeouts that take a minute on a
 * real machine take no time here.
 *
 * The file is included, not linked, so each case can put the table back to its
 * initial state; the kernel has one boot per run and needs no such thing.
 */

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long n);

/* The kernel headers define the fixed-width types themselves. */
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"
#include "../../rach2/sys/network/tcp/tcp.h"
#include "../../rach2/sys/network/driver/netdev.h"
#include "../../rach2/sys/network/socket/socket.h"

/* ---- the clock, and the layers above and below ---- */

volatile uint32_t kernel_ticks;

/**
 * Return the microsecond clock TCP times its round trips by, for `ss -i`: 10000
 * to a tick.
 */
uint32_t timer_now_us(void)
{
    return kernel_ticks * 10000u;
}

#define MAX_SEGS 64

static struct {
    uint32_t dst;
    uint8_t proto;
    uint16_t len;
    uint8_t data[128];
} segs[MAX_SEGS];       /* the first MAX_SEGS segments ip_tx sent */
static unsigned n_segs; /* the segments sent, MAX_SEGS and past it */

static unsigned cb_connected, cb_fin, cb_writable, cb_deliver, cb_accept;
static unsigned delivered_bytes; /* delivered in order or released */
static unsigned held_bytes;      /* kept past a hole */

/* How many transmits go through before the driver refuses, how many it
 * refuses then, and with which error: a full ring's -ENOBUFS, or a
 * permanent one. */
static int ip_tx_accept;
static int ip_tx_refuse;
static int ip_tx_refuse_err = -ENOBUFS;

/* The stream as sent, put back together by sequence number from every
 * segment's whole payload, so a byte copied from the wrong place in the
 * ring shows wherever it lies in a segment. */
#define STREAM_MAX 80000
static uint8_t stream_got[STREAM_MAX];
static uint32_t stream_base; /* the sequence number of stream_got[0] */
static int stream_on;        /* nonzero while ip_tx fills stream_got */
uint32_t read32be(const uint8_t *p);

/**
 * Record a segment sent, unless the driver model fails it: after
 * `ip_tx_accept` transmits, the next `ip_tx_refuse` fail with
 * `ip_tx_refuse_err`. While `stream_on` is set, a TCP segment's payload is also
 * put in `stream_got` at its offset from `stream_base`. Returns 0 for a segment
 * sent.
 */
int ip_tx(uint32_t dst, uint8_t proto, const uint8_t *payload, uint16_t len, uint8_t ttl)
{
    unsigned i;

    (void)ttl;

    if (ip_tx_accept > 0) {
        ip_tx_accept--;
    } else if (ip_tx_refuse > 0) {
        ip_tx_refuse--;
        return ip_tx_refuse_err;
    }
    if (stream_on && proto == 6 && len > TCP_HDR_SIZE) {
        uint32_t off = read32be(payload + 4) - stream_base;
        uint32_t n = (uint32_t)len - TCP_HDR_SIZE;

        if (off + n <= STREAM_MAX) {
            for (i = 0; i < n; i++) {
                stream_got[off + i] = payload[TCP_HDR_SIZE + i];
            }
        }
    }
    if (n_segs < MAX_SEGS) {
        segs[n_segs].dst = dst;
        segs[n_segs].proto = proto;
        segs[n_segs].len = len;
        for (i = 0; i < len && i < sizeof(segs[0].data); i++) {
            segs[n_segs].data[i] = payload[i];
        }
    }
    n_segs++;
    return 0;
}

/** Count a connection that completed or ended. */
void socket_tcp_connected(struct socket *sock)
{
    (void)sock;
    cb_connected++;
}

/** Count the peer's FIN or the connection's end. */
void socket_tcp_fin(struct socket *sock)
{
    (void)sock;
    cb_fin++;
}

/** Count a report of freed send-ring space. */
void socket_tcp_writable(struct socket *sock)
{
    (void)sock;
    cb_writable++;
}

/** Take and count all `len` bytes delivered in order. */
uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len)
{
    (void)sock;
    (void)data;
    cb_deliver++;
    delivered_bytes += len;
    return len;
}

/** Take and count all `len` bytes that arrived past a hole. */
uint16_t socket_tcp_hold(struct socket *sock, uint32_t offset, const uint8_t *data, uint16_t len)
{
    (void)sock;
    (void)offset;
    (void)data;
    held_bytes += len;
    return len;
}

/** Count `n` held bytes made readable as delivered. */
void socket_tcp_release(struct socket *sock, uint32_t n)
{
    (void)sock;
    delivered_bytes += n;
}

/** Count a connection queued on a listener for accept. */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock)
{
    (void)listen_sock;
    (void)conn_sock;
    cb_accept++;
}

/** Release a socket: a no-op. */
void socket_unref(struct socket *sock)
{
    (void)sock;
}

/* The one port another socket has bound, which an ephemeral port must pass
   over; 0 for none. */
static uint16_t bound_elsewhere;

/** Report whether `port` is the one another socket has bound. */
int socket_port_bound(uint16_t port)
{
    return bound_elsewhere != 0 && port == bound_elsewhere;
}

/* A passive open's socket, from a pool of four, which the listener rows
   need no more of. */
static struct socket sock_pool[4];
static unsigned sock_pooled; /* the sockets of the pool given out */

/**
 * Return the next socket of the pool, cleared, of type `type` and with one
 * reference, or NULL when the pool is used up.
 */
struct socket *socket_alloc(int type)
{
    struct socket *s;

    if (sock_pooled >= sizeof(sock_pool) / sizeof(sock_pool[0])) {
        return (struct socket *)0;
    }
    s = &sock_pool[sock_pooled++];
    k_memset(s, 0, sizeof(*s));
    s->type = type;
    s->refcount = 1;
    return s;
}

uint32_t net_ip_addr = 0x0A00020Fu; /* 10.0.2.15 */

/**
 * Read the big-endian 16-bit field at `p`.
 *
 * This and the three functions after it are the byte-order helpers of
 * net_core.c, written out here rather than bringing in the stack's bring-up,
 * which shares their file.
 */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/** Read the big-endian 32-bit field at `p`. */
uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Store `v` at `p` in network byte order. */
void write16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/** Store `v` at `p` in network byte order. */
void write32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/**
 * Return the Internet checksum of `len` bytes at `buf`: the IP header checksum,
 * which `tcp_checksum` reuses for the pseudo-header. Its own file, ip.c, would
 * bring the whole IP layer with it.
 */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    uint32_t sum = 0;
    uint16_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)(((uint32_t)buf[i] << 8) | buf[i + 1]);
    }
    if (i < len) {
        sum += (uint32_t)((uint32_t)buf[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* The interrupt flag net_lock saves and clears, and an interrupt a row arms
   to arrive just before the next net_lock masks interrupts. */
static uint32_t irq_enabled = 1;
static void (*before_net_lock)(void);

/**
 * Take the network lock: when interrupts are enabled, run the interrupt
 * `before_net_lock` once, as one arriving just before the mask would; then
 * clear `irq_enabled` and return what it was.
 */
uint32_t net_lock(void)
{
    uint32_t flags = irq_enabled;
    void (*interrupt)(void) = before_net_lock;

    irq_enabled = 0;
    if (flags && interrupt) {
        before_net_lock = 0;
        interrupt();
    }
    return flags;
}

/** Release the network lock: put back the interrupt flag `flags`. */
void net_unlock(uint32_t flags)
{
    irq_enabled = flags;
}

/** Discard a kernel log record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/**
 * Report whether `ip` is the machine's: the one interface's address, which
 * every case here is written against, or an address of 127/8.
 */
int ip_is_local(uint32_t ip)
{
    return ip != 0 && (ip == net_ip_addr || (ip & 0xFF000000u) == 0x7F000000u);
}

/** Return the source address for `dst`: 127.0.0.1 for 127/8, else the
 * interface's address. */
uint32_t ip_source(uint32_t dst)
{
    return (dst & 0xFF000000u) == 0x7F000000u ? 0x7F000001u : net_ip_addr;
}

/** Send a segment from `src`, as ip_tx does. */
int ip_tx_from(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload, uint16_t len,
               uint8_t ttl)
{
    (void)src;
    return ip_tx(dst, proto, payload, len, ttl);
}

/**
 * Return a constant in place of the kernel's random words, from which `tcp.c`
 * draws the key of its initial sequence numbers. Every case reads a
 * connection's number from the segment that has it, so any key serves;
 * tcp_reset_hosttest.c checks the numbers themselves.
 */
uint32_t krandom_u32(void)
{
    return 0x52414348u;
}

#include "../../rach2/sys/network/tcp/tcp.c"

/* The counters /proc/net/snmp reports, which net_core.c defines. */
struct net_mib net_mib;

/* ---- the cases ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

#define PEER      0x0A000202u /* 10.0.2.2 */
#define TCP_PROTO 6

/* The flags in a segment this test builds or reads. */
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

/** Free every control block, forget every segment and callback, and set
 * the clock to tick 1000. */
static void reset_all(void)
{
    unsigned i;

    n_segs = 0;
    cb_connected = cb_fin = cb_writable = cb_deliver = cb_accept = 0;
    delivered_bytes = 0;
    kernel_ticks = 1000;
    while (tcbs) {
        tcb_free(tcbs);
    }
}

/* The MSS option a peer's SYN has, as a real peer's does; a row sets it
   lower to see the stack keep to it. */
static uint16_t peer_mss = TCP_MSS;
/* The window scale option a peer's SYN has, or -1 for none, as every row but
   window_scale_rows has it. */
static int peer_wscale = -1;
/* Whether a peer's SYN permits selective acknowledgements. */
static int peer_sack = 0;

/**
 * Give this stack a segment from the peer's port `sport` to `dport`, with the
 * numbers `seq` and `ack`, `flags`, `window` and the `plen` bytes of `payload`.
 * A SYN has the MSS option of `peer_mss`, the window scale of `peer_wscale`
 * unless it is -1, and the SACK-permitted option when `peer_sack` is set.
 *
 * The checksum is computed as the sender computes it: the receiver verifies it
 * and drops a segment with a wrong one, and every case would then be checking
 * that.
 */
static void rx(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
               uint16_t window, const char *payload, uint16_t plen)
{
    uint8_t seg[128];
    uint16_t hdr = 20;
    unsigned i;

    write16be(seg + 0, sport);
    write16be(seg + 2, dport);
    write32be(seg + 4, seq);
    write32be(seg + 8, ack);
    seg[12] = 0x50; /* five words of header */
    seg[13] = flags;
    write16be(seg + 14, window);
    write16be(seg + 16, 0);
    write16be(seg + 18, 0); /* the urgent pointer */
    if (flags & F_SYN) {
        seg[12] = 0x60;
        seg[20] = 2;
        seg[21] = 4;
        write16be(seg + 22, peer_mss);
        hdr = 24;
    }
    if ((flags & F_SYN) && peer_wscale >= 0) {
        seg[12] = 0x70;
        seg[24] = 1;
        seg[25] = 3;
        seg[26] = 3;
        seg[27] = (uint8_t)peer_wscale;
        hdr = 28;
    }
    if ((flags & F_SYN) && peer_sack) {
        seg[hdr] = 1;
        seg[hdr + 1] = 1;
        seg[hdr + 2] = 4;
        seg[hdr + 3] = 2;
        hdr = (uint16_t)(hdr + 4);
        seg[12] = (uint8_t)((hdr / 4) << 4);
    }
    for (i = 0; i < plen; i++) {
        seg[hdr + i] = (uint8_t)payload[i];
    }
    write16be(seg + 16, tcp_checksum(PEER, net_ip_addr, seg, (uint16_t)(hdr + plen)));
    tcp_rx(PEER, net_ip_addr, seg, (uint16_t)(hdr + plen));
}

/**
 * Return the flags of the segment this stack sent last, or 0 when none is
 * recorded.
 */
static uint8_t last_flags(void)
{
    if (n_segs == 0 || n_segs > MAX_SEGS) {
        return 0;
    }
    return segs[n_segs - 1].data[13];
}

/**
 * Open a connection to the peer's `port` and complete the handshake with a
 * 65535 window. `*peer_seq` is the peer's initial sequence number and is set
 * past its SYN; `*lport` gets the local port. Returns the control block.
 */
static struct tcb *open_conn(struct socket *sock, uint16_t port, uint32_t *peer_seq,
                             uint16_t *lport)
{
    struct tcb *conn;
    uint32_t iss;

    k_memset(sock, 0, sizeof(*sock));
    n_segs = 0;
    CHECK("a connection can be opened", tcp_connect(sock, PEER, port) == 0);
    conn = sock->tcb;
    iss = read32be(segs[0].data + 4);
    *lport = read16be(segs[0].data + 0);
    rx(port, *lport, *peer_seq, iss + 1, (uint8_t)(F_SYN | F_ACK), 65535, (const char *)0, 0);
    (*peer_seq)++;
    n_segs = 0;
    return conn;
}

/**
 * Grow a connection's congestion window to `want` bytes: 40000 bytes are
 * offered and every segment is acknowledged on its own, each acknowledgement
 * adding a segment to the window, and what remains is acknowledged whole at the
 * end, so the ring is empty and nothing is in flight.
 */
static void grow_window(struct socket *sock, struct tcb *conn, uint16_t port, uint16_t lport,
                        uint32_t peer_seq, uint32_t want)
{
    static uint8_t bytes[40000];
    unsigned k = 0;

    n_segs = 0;
    CHECK("the ring takes 40000 bytes to grow the window",
          tcp_send(sock, bytes, (int)sizeof(bytes)) == (int)sizeof(bytes));
    while (conn->cwnd < want && k < n_segs && k < MAX_SEGS) {
        uint32_t end = read32be(segs[k].data + 4) + (segs[k].len - TCP_HDR_SIZE);

        rx(port, lport, peer_seq, end, F_ACK, 65535, (const char *)0, 0);
        k++;
    }
    while (conn->send_head != conn->send_tail || conn->snd_una != conn->snd_nxt) {
        rx(port, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);
    }
    CHECK("the window grew as asked", conn->cwnd >= want);
    n_segs = 0;
}

static struct tcb *reset_on_lock; /* the connection receive_reset resets */

/**
 * Give `reset_on_lock` a reset from its peer at its next sequence number; armed
 * as `before_net_lock`, it arrives just before the next net_lock.
 */
static void receive_reset(void)
{
    rx(reset_on_lock->remote_port, reset_on_lock->local_port, reset_on_lock->rcv_nxt,
       reset_on_lock->snd_nxt, F_RST, 65535, NULL, 0);
}

/**
 * Check that tcp_send rejects a missing socket or connection, a negative length
 * and a closed connection; takes zero bytes, nothing into a full ring, and data
 * in ESTABLISHED and CLOSE_WAIT; and puts the interrupt flag back each time,
 * enabled or not. A reset that arrives just before tcp_send masks interrupts
 * fails the send, closes the connection, and leaves the ring and the wire
 * untouched.
 */
static void test_send_checks(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t peer_seq = 5000;
    uint16_t lport;
    uint32_t enabled;
    const uint8_t byte = 'x';

    reset_all();
    conn = open_conn(&sock, 80, &peer_seq, &lport);
    for (enabled = 0; enabled <= 1; enabled++) {
        struct socket empty;

        irq_enabled = enabled;
        k_memset(&empty, 0, sizeof(empty));
        CHECK("send rejects a missing socket", tcp_send(NULL, &byte, 1) == -1);
        CHECK("missing socket restores interrupt state", irq_enabled == enabled);
        CHECK("send rejects a missing connection", tcp_send(&empty, &byte, 1) == -1);
        CHECK("missing connection restores interrupt state", irq_enabled == enabled);
        CHECK("send rejects a negative length", tcp_send(&sock, &byte, -1) == -1);
        CHECK("negative length restores interrupt state", irq_enabled == enabled);
        conn->state = TCP_CLOSED;
        CHECK("send rejects a closed connection", tcp_send(&sock, &byte, 1) == -1);
        CHECK("closed connection restores interrupt state", irq_enabled == enabled);
        conn->state = TCP_ESTABLISHED;
        CHECK("send accepts zero bytes", tcp_send(&sock, &byte, 0) == 0);
        CHECK("zero length restores interrupt state", irq_enabled == enabled);
        conn->send_tail = conn->send_ack + TCP_RING_SIZE;
        CHECK("send returns zero for a full ring", tcp_send(&sock, &byte, 1) == 0);
        CHECK("full ring restores interrupt state", irq_enabled == enabled);
        conn->send_tail = conn->send_ack;
        CHECK("send accepts established data", tcp_send(&sock, &byte, 1) == 1);
        CHECK("established send restores interrupt state", irq_enabled == enabled);
        rx(80, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, NULL, 0);
        conn->state = TCP_CLOSE_WAIT;
        CHECK("send accepts close-wait data", tcp_send(&sock, &byte, 1) == 1);
        CHECK("close-wait send restores interrupt state", irq_enabled == enabled);
        rx(80, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, NULL, 0);
        conn->state = TCP_ESTABLISHED;
    }
    reset_all();
    conn = open_conn(&sock, 80, &peer_seq, &lport);
    reset_on_lock = conn;
    before_net_lock = receive_reset;
    CHECK("send rejects a reset before masking", tcp_send(&sock, &byte, 1) == -1);
    CHECK("the pending reset was delivered", before_net_lock == NULL);
    CHECK("reset closes the attached connection",
          sock.tcb == conn && conn->state == TCP_CLOSED && conn->so_error == ECONNRESET);
    CHECK("reset before send leaves the ring unchanged",
          conn->send_ack == 0 && conn->send_head == 0 && conn->send_tail == 0);
    CHECK("reset before send transmits nothing", n_segs == 0);
    CHECK("reset refusal restores interrupts", irq_enabled == 1);
    reset_on_lock = NULL;
    reset_all();
}

/**
 * On the open connection `conn` from local port `lport` to the peer's port 80,
 * whose next byte is `peer_seq`, with this side's initial sequence number
 * `iss`: a segment past a hole is kept but not delivered, and the
 * acknowledgement still names the byte the receiver is waiting for; the segment
 * that fills the hole is delivered with the one kept after it, and the
 * acknowledgement covers both; and a resend that starts before the byte awaited
 * and runs past it delivers only the new bytes. Returns the peer's next byte
 * after them.
 */
static uint32_t hole_rows(struct tcb *conn, uint16_t lport, uint32_t peer_seq, uint32_t iss)
{
    cb_deliver = 0;
    delivered_bytes = 0;
    held_bytes = 0;
    n_segs = 0;
    rx((uint16_t)80, lport, peer_seq + 4, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "ijkl", 4);
    CHECK("a segment past a hole is kept and not delivered",
          delivered_bytes == 0 && held_bytes == 4);
    CHECK("and the acknowledgement still asks for the hole",
          n_segs >= 1 && read32be(segs[n_segs - 1].data + 8) == peer_seq);

    cb_deliver = 0;
    delivered_bytes = 0;
    n_segs = 0;
    rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "efgh", 4);
    CHECK("the segment that fills the hole is delivered with the one kept after it",
          delivered_bytes == 8);
    CHECK("and the acknowledgement covers both",
          n_segs >= 1 && read32be(segs[n_segs - 1].data + 8) == peer_seq + 8);
    peer_seq += 8;

    delivered_bytes = 0;
    rx((uint16_t)80, lport, peer_seq - 2, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "klmn", 4);
    CHECK("a segment that overlaps the data taken delivers only what is new",
          delivered_bytes == 2 && conn->rcv_nxt == peer_seq + 2);
    return peer_seq + 2;
}

/**
 * Check that a peer whose SYN asks for smaller segments, as one behind a PPPoE
 * line's router does, gets no segment larger than it asked for, and that the
 * first burst is four of them.
 */
static void peer_mss_row(void)
{
    struct socket sock;
    uint32_t iss, peer_seq = 8000;
    uint16_t lport;
    static uint8_t bytes[8000];
    unsigned j, oversize = 0;

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    CHECK("a connection to a peer of a smaller MSS can be opened",
          tcp_connect(&sock, PEER, 87) == 0);
    iss = read32be(segs[0].data + 4);
    lport = read16be(segs[0].data + 0);
    peer_mss = 1400;
    rx((uint16_t)87, lport, peer_seq, iss + 1, (uint8_t)(F_SYN | F_ACK), 65535, (const char *)0, 0);
    peer_mss = TCP_MSS;
    n_segs = 0;
    CHECK("the ring takes 8000 bytes", tcp_send(&sock, bytes, (int)sizeof(bytes)) == 8000);
    for (j = 0; j < n_segs && j < MAX_SEGS; j++) {
        if (segs[j].len - TCP_HDR_SIZE > 1400) {
            oversize++;
        }
    }
    CHECK("the first burst is four segments of the peer's size",
          n_segs == 4 && oversize == 0 && segs[0].len - TCP_HDR_SIZE == 1400);
}

/**
 * Check that a router's report that a segment in flight was too big for a link
 * of 1480 bytes shrinks the connection's segments to 1440 and sends the oldest
 * again at that size, and that a report for a sequence number not in flight, or
 * one that would make them larger, changes nothing.
 */
static void path_mtu_row(void)
{
    struct socket sock;
    uint32_t iss;
    uint16_t lport;
    static uint8_t bytes[4000];

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    CHECK("a connection for the path MTU can be opened", tcp_connect(&sock, PEER, 89) == 0);
    iss = read32be(segs[0].data + 4);
    lport = read16be(segs[0].data + 0);
    rx((uint16_t)89, lport, 5000, iss + 1, (uint8_t)(F_SYN | F_ACK), 65535, (const char *)0, 0);
    n_segs = 0;
    CHECK("it sends 4000 bytes", tcp_send(&sock, bytes, (int)sizeof(bytes)) == 4000);
    CHECK("in segments of 1460", n_segs >= 1 && segs[0].len - TCP_HDR_SIZE == TCP_MSS);
    tcp_path_mtu(net_ip_addr, PEER, lport, 89, iss + 1 + 8000u, 1480);
    CHECK("a report for a sequence number not in flight changes nothing",
          sock.tcb->snd_mss == TCP_MSS);
    tcp_path_mtu(net_ip_addr, PEER, lport, 89, iss + 1, 1600);
    CHECK("a report of a larger MTU changes nothing", sock.tcb->snd_mss == TCP_MSS);
    n_segs = 0;
    tcp_path_mtu(net_ip_addr, PEER, lport, 89, iss + 1, 1480);
    CHECK("a report for a segment in flight shrinks the segments to the link",
          sock.tcb->snd_mss == 1440);
    CHECK("and sends the oldest again at that size", n_segs >= 1 &&
                                                         read32be(segs[0].data + 4) == iss + 1 &&
                                                         segs[0].len - TCP_HDR_SIZE == 1440);
}

/**
 * Check a connection whose SYN offers a window scale: a peer that answers with
 * a scale of its own gets windows scaled both ways, and one that answers
 * without gets neither, and never more than 65535 offered.
 */
static void window_scale_rows(void)
{
    struct socket sock;
    uint32_t iss;
    uint16_t lport;
    int scale;

    for (scale = 7; scale >= -1; scale -= 8) {
        reset_all();
        k_memset(&sock, 0, sizeof(sock));
        CHECK("a connection that scales its window can be opened",
              tcp_connect(&sock, PEER, 88) == 0);
        CHECK("and its SYN offers the scale its 512 KiB ring needs",
              segs[0].len == 32 && segs[0].data[25] == 3 && segs[0].data[27] == 4 &&
                  read16be(segs[0].data + 14) == 65535);
        iss = read32be(segs[0].data + 4);
        lport = read16be(segs[0].data + 0);
        peer_wscale = scale;
        rx((uint16_t)88, lport, 3000, iss + 1, (uint8_t)(F_SYN | F_ACK), 1000, (const char *)0, 0);
        peer_wscale = -1;
        CHECK("the SYN's window is not scaled", sock.tcb->snd_wnd == 1000);
        n_segs = 0;
        rx((uint16_t)88, lport, 3001, iss + 1, F_ACK, 1000, (const char *)0, 0);
        if (scale > 0) {
            CHECK("a scaling peer's window is its field times its scale",
                  sock.tcb->snd_wnd == 1000u << scale);
        } else {
            CHECK("a peer that does not scale gives its field as it is",
                  sock.tcb->snd_wnd == 1000u);
        }
        rx((uint16_t)88, lport, 3001, iss + 1, (uint8_t)(F_ACK | F_PSH), 1000, "data", 4);
        CHECK("and the acknowledgement offers the ring, which the stub delivery leaves empty, at "
              "this side's scale, or 65535",
              n_segs >= 1 && read16be(segs[n_segs - 1].data + 14) ==
                                 (scale > 0 ? TCP_RECV_RING_LARGE >> 4 : 65535u));
    }
}

/**
 * Return the option of kind `kind` in the segment this stack sent, `seg` of
 * `len` bytes, or NULL when it has none.
 */
static const uint8_t *sent_option(const uint8_t *seg, uint16_t len, uint8_t kind)
{
    uint16_t hdr = (uint16_t)((seg[12] >> 4) * 4), i = 20;

    while (i < hdr && i < len && seg[i] != 0) {
        if (seg[i] == 1) {
            i++;
            continue;
        }
        if (seg[i] == kind) {
            return seg + i;
        }
        if (i + 1u >= hdr || seg[i + 1] < 2) {
            break;
        }
        i = (uint16_t)(i + seg[i + 1]);
    }
    return 0;
}

/**
 * Return 1 when the acknowledgement sent last names the SACK blocks `want`, `n`
 * pairs of start and end, in that order, and nothing else; else 0.
 */
static int sacks_are(const uint32_t *want, unsigned n)
{
    const uint8_t *o;
    unsigned k;

    if (n_segs == 0) {
        return 0;
    }
    o = sent_option(segs[n_segs - 1].data, segs[n_segs - 1].len, 5);
    if (n == 0) {
        return o == 0;
    }
    if (!o || o[1] != 2 + 8 * n) {
        return 0;
    }
    for (k = 0; k < 2 * n; k++) {
        if (read32be(o + 2 + 4 * k) != want[k]) {
            return 0;
        }
    }
    return 1;
}

/** Return the control block of a socket a row opened. */
static struct tcb *conn_of(struct socket *sock)
{
    return sock->tcb;
}

/**
 * Check selective acknowledgements: this side's SYN permits them, and with a
 * peer whose SYN did too, an acknowledgement sent while data past a hole is
 * held names that data, the blocks of the segments that came last first, newest
 * first, until the holes are filled; a segment past a table already full is not
 * named; a peer that did not permit them gets none, and a listener's answer
 * permits them only to a SYN that did.
 */
static void sack_rows(void)
{
    struct socket sock, lsock;
    uint32_t iss, p = 4000, want[8];
    uint16_t lport;
    int sacking, k;

    for (sacking = 1; sacking >= 0; sacking--) {
        reset_all();
        k_memset(&sock, 0, sizeof(sock));
        CHECK("a connection to try selective acknowledgements can be opened",
              tcp_connect(&sock, PEER, 89) == 0);
        CHECK("and its SYN permits them", sent_option(segs[0].data, segs[0].len, 4) != 0);
        iss = read32be(segs[0].data + 4);
        lport = read16be(segs[0].data + 0);
        peer_sack = sacking;
        rx((uint16_t)89, lport, p, iss + 1, (uint8_t)(F_SYN | F_ACK), 65535, (const char *)0, 0);
        peer_sack = 0;
        n_segs = 0;
        rx((uint16_t)89, lport, p + 11, iss + 1, F_ACK, 65535, "late1", 5);
        want[0] = p + 11;
        want[1] = p + 16;
        CHECK(sacking ? "a segment past a hole is named in the acknowledgement"
                      : "a peer that did not permit them is sent none",
              sacks_are(want, sacking ? 1 : 0));
        if (!sacking) {
            break;
        }
        rx((uint16_t)89, lport, p + 31, iss + 1, F_ACK, 65535, "late2", 5);
        want[0] = p + 31;
        want[1] = p + 36;
        want[2] = p + 11;
        want[3] = p + 16;
        CHECK("the block of the segment that came last is named first", sacks_are(want, 2));
        rx((uint16_t)89, lport, p + 1, iss + 1, F_ACK, 65535, "0123456789", 10);
        want[0] = p + 31;
        want[1] = p + 36;
        CHECK("a filled hole is no longer named",
              read32be(segs[n_segs - 1].data + 8) == p + 16 && sacks_are(want, 1));
        rx((uint16_t)89, lport, p + 16, iss + 1, F_ACK, 65535, "abcdefghijklmno", 15);
        CHECK("and with every hole filled the acknowledgement names nothing",
              read32be(segs[n_segs - 1].data + 8) == p + 36 && sacks_are(want, 0));
        for (k = 0; k < 5; k++) {
            rx((uint16_t)89, lport, p + 46 + 10 * k, iss + 1, F_ACK, 65535, "abcde", 5);
        }
        for (k = 0; k < 4; k++) {
            want[2 * k] = p + 86 - 10 * k;
            want[2 * k + 1] = p + 91 - 10 * k;
        }
        CHECK("the blocks of the last four segments kept are named, the newest first",
              sacks_are(want, 4));
        for (k = 5; k < 9; k++) {
            rx((uint16_t)89, lport, p + 46 + 10 * k, iss + 1, F_ACK, 65535, "abcde", 5);
        }
        for (k = 0; k < 4; k++) {
            want[2 * k] = p + 116 - 10 * k;
            want[2 * k + 1] = p + 121 - 10 * k;
        }
        CHECK("a segment past a full table is not named, the last one kept is",
              conn_of(&sock)->ooo_count == TCP_OOO_RANGES && sacks_are(want, 4));
    }

    for (sacking = 1; sacking >= 0; sacking--) {
        reset_all();
        k_memset(&lsock, 0, sizeof(lsock));
        CHECK("a listener for selective acknowledgements", tcp_listen(&lsock, 90) == 0);
        peer_sack = sacking;
        rx(5555, 90, 7000, 0, F_SYN, 65535, (const char *)0, 0);
        peer_sack = 0;
        CHECK(sacking ? "a listener's answer permits them to a SYN that did"
                      : "and not to a SYN that did not",
              n_segs == 1 && (last_flags() & F_SYN) != 0 &&
                  (sent_option(segs[0].data, segs[0].len, 4) != 0) == sacking);
    }
    reset_all();
}

/**
 * Check that a window reopened by a reader that shut down its writing half is
 * announced in FIN_WAIT_1 and FIN_WAIT_2, as in ESTABLISHED: such a reader goes
 * on reading, and without the announcement the peer waits for its persist
 * timer.
 */
static void half_close_rows(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t peer_seq = 6000, fin_seq;
    uint16_t lport;

    reset_all();
    conn = open_conn(&sock, 91, &peer_seq, &lport);
    tcp_shutdown_send(&sock);
    CHECK("shutting down the writing half sends the FIN",
          n_segs == 1 && (last_flags() & F_FIN) != 0 && conn->state == TCP_FIN_WAIT_1);
    fin_seq = read32be(segs[0].data + 4);
    conn->rcv_wnd = 0;
    n_segs = 0;
    tcp_window_update(&sock);
    CHECK("a window reopened in FIN_WAIT_1 is announced",
          n_segs == 1 && read16be(segs[0].data + 14) != 0);
    rx(91, lport, peer_seq, fin_seq + 1, F_ACK, 65535, (const char *)0, 0);
    CHECK("the peer's acknowledgement of the FIN leads to FIN_WAIT_2",
          conn->state == TCP_FIN_WAIT_2);
    conn->rcv_wnd = 0;
    n_segs = 0;
    tcp_window_update(&sock);
    CHECK("and a window reopened there is announced",
          n_segs == 1 && read16be(segs[0].data + 14) != 0);
    reset_all();
}

/**
 * Check that an ephemeral port passes over one another socket has bound, as it
 * passes over a port a control block uses.
 */
static void ephemeral_rows(void)
{
    reset_all();
    next_ephemeral = 50000;
    bound_elsewhere = 50000;
    CHECK("an ephemeral port passes over a port another socket has bound",
          tcp_bind_ephemeral() == 50001);
    bound_elsewhere = 0;
}

/**
 * Run the cases, each against a table put back to its initial state:
 *
 *   * the rows of test_send_checks, then forty control blocks at once, one
 *     freed from the middle of the list and the rest kept;
 *   * a segment for a port nobody listens on is answered with a reset, which
 *     tells the peer to stop retransmitting, and a reset for such a port is not
 *     answered, or two machines would reset each other for ever;
 *   * a connection opened and carried, measured against the sequence numbers
 *     its handshake settles: data in order is delivered once, and the same
 *     segment again is acknowledged again, so that a peer that lost the first
 *     acknowledgement stops asking; the rows of hole_rows; a zero window keeps
 *     data offered from going out until the window reopens, which a check of
 *     the stored window alone would pass in a stack that ignored it; and the
 *     peer's FIN closes the reading half and is acknowledged;
 *   * a ring's worth of data: the ring takes it in one call, the bursts are
 *     bounded by the windows, every byte leaves in segments of at most one MSS,
 *     and a second transfer takes the ring's indexes past 65536 with every byte
 *     where the stream has it, so a wrap that copied from the wrong place would
 *     show. A transmit the driver refuses for a full ring stays in the ring for
 *     the next pump, the tick's or an acknowledgement's, rather than being
 *     counted as sent; a permanent error counts the segment as sent and leaves
 *     it to the retransmission timer; and a connection whose driver refuses
 *     every transmit is given up after the bound rather than kept for ever;
 *   * the rows of peer_mss_row, window_scale_rows, sack_rows, half_close_rows,
 *     ephemeral_rows and path_mtu_row;
 *   * a zero window that closes over a refused segment is flow control, not a
 *     continuing refusal: the connection is kept past the bound and the segment
 *     goes when the window reopens. A connection last in the list behind four
 *     that a zero window holds is still pumped, since the tick's pumps rotate;
 *     the list is newest first, so the first opened is the last pumped;
 *   * a burst lost whole: the timer fires once, and from then on the
 *     acknowledgements drive the resend, one segment at the timeout, two when
 *     it is acknowledged, then a segment more per acknowledgement, until
 *     everything sent before the loss is on the wire again, with no further
 *     wait on the timer and every segment sent again once, in order. The
 *     timeout the loss doubled is measured again once new data is acknowledged;
 *   * one segment lost from the middle of a burst: the two duplicate
 *     acknowledgements before the third send nothing; the third sends the
 *     segment again at once, without a tick, in a recovery with the window
 *     halved plus the three segments the duplicates say have left; a further
 *     duplicate adds a segment; and the acknowledgement of everything ends the
 *     recovery at the threshold;
 *   * two segments lost from one burst: the third duplicate sends the first
 *     again, the acknowledgement that then stops at the second hole sends the
 *     second again at once, in the same recovery, and the acknowledgement of
 *     everything ends it;
 *   * a close with a lost burst in flight: the FIN waits behind the data, the
 *     recovery goes on from the acknowledgements in FIN_WAIT_1, and a segment
 *     the driver refuses there is sent by the tick;
 *   * a peer whose window closes during a timeout's recovery: the resend stops
 *     at the window, the zero-window probe timer runs, a probe goes out, and
 *     the window's reopening resumes the recovery. A reset in the middle of a
 *     recovery ends it with the connection;
 *   * a fast resend the driver refuses is owed, and the tick's pump sends it
 *     before anything else. Outside a recovery the mark that keeps a finished
 *     recovery's duplicates from beginning another moves up with each
 *     acknowledgement;
 *   * a close from the peer with data still going out: the recovery after a
 *     timeout runs through the acknowledgements in CLOSE_WAIT with the window
 *     growing, as it does in ESTABLISHED;
 *   * a round-trip sample ends at the acknowledgement that covers the sampled
 *     segment, not at the first acknowledgement after it;
 *   * a peer that stops answering a SYN: the stack retransmits with a doubling
 *     timer and gives up, rather than holding the connection for ever. The
 *     timer counts ticks and `tcp_tick` is one tick, so this is the timer
 *     running rather than the clock jumping: the five backoffs together are
 *     about six thousand ticks. The gaps between the attempts are checked,
 *     since a stack that retried every tick would also give up in the end. The
 *     control block stays until the socket lets go of it, as a program still
 *     holding the descriptor expects; the socket is told that the connection
 *     ended.
 *
 * Returns the number of failed checks, at most 255.
 */
int main(void)
{
    struct tcb *t;
    unsigned i;

    tcp_init();
    test_send_checks();
    reset_all();
    t = 0;
    for (i = 0; i < 40; i++) {
        struct tcb *b = tcb_alloc();

        CHECK("every block asked for can be had", b != 0);
        if (i == 20) {
            t = b;
        }
    }
    tcb_free(t);
    for (i = 0, t = tcbs; t; t = t->next) {
        i++;
    }
    CHECK("thirty-nine remain after one is freed", i == 39);

    reset_all();
    rx(4000, 9999, 100, 0, F_SYN, 4096, (const char *)0, 0);
    CHECK("a SYN for a closed port is answered", n_segs == 1);
    CHECK("and the answer is a reset", (last_flags() & F_RST) != 0);

    reset_all();
    rx(4000, 9999, 100, 0, F_RST, 0, (const char *)0, 0);
    CHECK("a reset for a closed port is not answered", n_segs == 0);

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t iss, peer_seq = 5000;
        uint16_t lport;

        reset_all();
        k_memset(&sock, 0, sizeof(sock));
        CHECK("a connection can be opened", tcp_connect(&sock, PEER, 80) == 0);
        conn = sock.tcb;
        CHECK("and it took a control block", conn != 0);
        CHECK("which sent a SYN", n_segs == 1 && (last_flags() & F_SYN) != 0);
        iss = read32be(segs[0].data + 4);
        lport = read16be(segs[0].data + 0);

        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_SYN | F_ACK), 4096, (const char *)0,
           0);
        CHECK("the handshake completes", cb_connected == 1);
        CHECK("and the connection is established", conn->state == TCP_ESTABLISHED);
        CHECK("and is acknowledged", (last_flags() & F_ACK) != 0);
        peer_seq++;

        n_segs = 0;
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "abcd", 4);
        CHECK("data in order is delivered", cb_deliver == 1 && delivered_bytes == 4);
        CHECK("and acknowledged", n_segs >= 1 && (last_flags() & F_ACK) != 0);
        CHECK("offering the ring clamped to the field, not zero",
              n_segs >= 1 && read16be(segs[n_segs - 1].data + 14) == 65535);

        cb_deliver = 0;
        delivered_bytes = 0;
        n_segs = 0;
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "abcd", 4);
        CHECK("the same data again is not delivered twice", delivered_bytes == 0);
        CHECK("and is acknowledged again", n_segs >= 1);
        peer_seq += 4;

        peer_seq = hole_rows(conn, lport, peer_seq, iss);

        rx((uint16_t)80, lport, peer_seq, iss + 1, F_ACK, 0, (const char *)0, 0);
        CHECK("a zero window is taken", conn->snd_wnd == 0);
        n_segs = 0;
        CHECK("data can be offered while the window is closed",
              tcp_send(&sock, (const uint8_t *)"payload!", 8) == 8);
        CHECK("and none of it goes out", n_segs == 0);
        rx((uint16_t)80, lport, peer_seq, iss + 1, F_ACK, 2048, (const char *)0, 0);
        CHECK("a reopened window is taken", conn->snd_wnd == 2048);
        CHECK("and the waiting data goes out", n_segs > 0);
        CHECK("carrying what was offered",
              n_segs > 0 && segs[n_segs - 1].len > 20 && segs[n_segs - 1].data[20] == 'p');

        n_segs = 0;
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_FIN), 4096, (const char *)0,
           0);
        CHECK("a FIN is reported to the socket", cb_fin == 1);
        CHECK("and acknowledged", n_segs >= 1 && (last_flags() & F_ACK) != 0);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t iss, peer_seq = 7000;
        uint16_t lport;
        static uint8_t big[60000];
        unsigned rounds = 0, sent_bytes = 0, j, oversize = 0;

        reset_all();
        k_memset(&sock, 0, sizeof(sock));
        for (j = 0; j < sizeof(big); j++) {
            big[j] = (uint8_t)j;
        }
        CHECK("a connection for a large transfer can be opened", tcp_connect(&sock, PEER, 82) == 0);
        conn = sock.tcb;
        iss = read32be(segs[0].data + 4);
        lport = read16be(segs[0].data + 0);
        rx((uint16_t)82, lport, peer_seq, iss + 1, (uint8_t)(F_SYN | F_ACK), 65535, (const char *)0,
           0);
        peer_seq++;
        n_segs = 0;
        stream_base = iss + 1;
        stream_on = 1;
        CHECK("the ring takes 60000 bytes in one call",
              tcp_send(&sock, big, (int)sizeof(big)) == (int)sizeof(big));
        CHECK("and the first burst is the initial congestion window",
              n_segs == TCP_INIT_CWND / TCP_MSS);
        while (conn->send_head != conn->send_tail && rounds < 40) {
            for (j = 0; j < n_segs && j < MAX_SEGS; j++) {
                sent_bytes += segs[j].len - TCP_HDR_SIZE;
                if (segs[j].len - TCP_HDR_SIZE > TCP_MSS) {
                    oversize++;
                }
            }
            n_segs = 0;
            rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);
            rounds++;
        }
        for (j = 0; j < n_segs && j < MAX_SEGS; j++) {
            sent_bytes += segs[j].len - TCP_HDR_SIZE;
        }
        CHECK("every byte left in a bounded number of rounds",
              sent_bytes == sizeof(big) && conn->send_head == conn->send_tail && rounds < 40);
        CHECK("and no segment was larger than an MSS", oversize == 0);
        rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);

        n_segs = 0;
        {
            unsigned wrong = 0, seen = 0;

            CHECK("the ring takes 20000 more bytes", tcp_send(&sock, big, 20000) == 20000);
            rounds = 0;
            for (;;) {
                for (j = 0; j < n_segs && j < MAX_SEGS; j++) {
                    uint32_t off = read32be(segs[j].data + 4) - iss - 1;
                    uint8_t want = (uint8_t)(off < sizeof(big) ? off : off - sizeof(big));

                    seen++;
                    if (segs[j].len > TCP_HDR_SIZE && segs[j].data[TCP_HDR_SIZE] != want) {
                        wrong++;
                    }
                }
                if (conn->send_head == conn->send_tail || rounds >= 40) {
                    break;
                }
                n_segs = 0;
                rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);
                rounds++;
            }
            CHECK("the stream's bytes cross the ring's end in order",
                  seen > 0 && wrong == 0 && conn->send_head == conn->send_tail);
            CHECK("and the ring's indexes are past its size", conn->send_tail > TCP_RING_SIZE);
            for (j = 0; j < STREAM_MAX; j++) {
                uint8_t want = big[j < sizeof(big) ? j : j - sizeof(big)];

                if (stream_got[j] != want) {
                    wrong++;
                }
            }
            CHECK(
                "every byte of the 80000 arrived where the stream has it, the ring's end included",
                wrong == 0);
            stream_on = 0;
            rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);
        }

        n_segs = 0;
        ip_tx_refuse = 1;
        CHECK("data is taken while the driver refuses", tcp_send(&sock, big, 3000) == 3000);
        CHECK("and nothing is counted as sent", n_segs == 0 && conn->send_head == conn->send_ack);
        CHECK("and the refusal is counted", conn->tx_refused == 1);
        kernel_ticks++;
        tcp_tick();
        CHECK("the tick sends what the driver refused",
              n_segs == 3 && conn->send_head == conn->send_ack + 3000);
        rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);

        n_segs = 0;
        ip_tx_accept = 1;
        ip_tx_refuse = 1;
        CHECK("data is taken when the driver refuses the second segment",
              tcp_send(&sock, big, 4000) == 4000);
        CHECK("the first segment went and the rest waits",
              n_segs == 1 && conn->send_head == conn->send_ack + TCP_MSS);
        n_segs = 0;
        rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);
        CHECK("the acknowledgement pumps the rest",
              n_segs == 2 && conn->send_head == conn->send_tail);
        rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);

        n_segs = 0;
        ip_tx_refuse = 1;
        ip_tx_refuse_err = -ENETUNREACH;
        CHECK("data is taken under a permanent error", tcp_send(&sock, big, 100) == 100);
        CHECK("and counted as sent, for the retransmission timer",
              conn->send_head == conn->send_tail && conn->rto_timer != 0);
        ip_tx_refuse_err = -ENOBUFS;
        rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);

        n_segs = 0;
        cb_fin = 0;
        ip_tx_refuse = 1000000;
        CHECK("data is taken while the driver refuses for good", tcp_send(&sock, big, 100) == 100);
        for (j = 0; j < TCP_REFUSE_GIVEUP_TICKS + 2 && conn->state == TCP_ESTABLISHED; j++) {
            kernel_ticks++;
            tcp_tick();
        }
        CHECK("the connection is given up after the bound",
              conn->state == TCP_CLOSED && cb_fin == 1 && j > TCP_REFUSE_GIVEUP_TICKS);
        ip_tx_refuse = 0;
    }

    peer_mss_row();
    window_scale_rows();
    sack_rows();
    half_close_rows();
    ephemeral_rows();
    path_mtu_row();

    {
        struct socket socks[6];
        struct tcb *conns[6];
        uint16_t lports[6];
        uint32_t iss6[6], peer_seq = 9000;
        unsigned c, j;
        static uint8_t bytes[2000];

        reset_all();
        for (c = 0; c < 6; c++) {
            k_memset(&socks[c], 0, sizeof(socks[c]));
            n_segs = 0;
            CHECK("a connection of six can be opened",
                  tcp_connect(&socks[c], PEER, (uint16_t)(90 + c)) == 0);
            conns[c] = socks[c].tcb;
            iss6[c] = read32be(segs[0].data + 4);
            lports[c] = read16be(segs[0].data + 0);
            rx((uint16_t)(90 + c), lports[c], peer_seq, iss6[c] + 1, (uint8_t)(F_SYN | F_ACK),
               65535, (const char *)0, 0);
        }
        peer_seq++;
        n_segs = 0;
        ip_tx_refuse = 1;
        CHECK("the newest connection's data is refused once",
              tcp_send(&socks[5], bytes, 100) == 100 && n_segs == 0);
        rx((uint16_t)95, lports[5], peer_seq, conns[5]->snd_nxt, F_ACK, 0, (const char *)0, 0);
        CHECK("then the peer closes its window over the refused segment", conns[5]->snd_wnd == 0);
        for (j = 0; j < TCP_REFUSE_GIVEUP_TICKS + 10; j++) {
            kernel_ticks++;
            tcp_tick();
        }
        CHECK("a zero window is not a refusal: the connection is kept",
              conns[5]->state == TCP_ESTABLISHED);
        n_segs = 0;
        rx((uint16_t)95, lports[5], peer_seq, conns[5]->snd_nxt, F_ACK, 65535, (const char *)0, 0);
        CHECK("and the segment goes when the window reopens",
              n_segs >= 1 && conns[5]->send_head == conns[5]->send_tail);
        rx((uint16_t)95, lports[5], peer_seq, conns[5]->snd_nxt, F_ACK, 65535, (const char *)0, 0);
        for (c = 1; c < 5; c++) {
            rx((uint16_t)(90 + c), lports[c], peer_seq, conns[c]->snd_nxt, F_ACK, 0,
               (const char *)0, 0);
            CHECK("a held connection takes data", tcp_send(&socks[c], bytes, 100) == 100);
        }
        rx((uint16_t)95, lports[5], peer_seq, conns[5]->snd_nxt, F_ACK, 0, (const char *)0, 0);
        CHECK("and the newest takes data behind a closed window too",
              conns[5]->snd_wnd == 0 && tcp_send(&socks[5], bytes, 100) == 100);
        n_segs = 0;
        ip_tx_refuse = 1;
        CHECK("the oldest connection's data is refused once",
              tcp_send(&socks[0], bytes, 100) == 100 && n_segs == 0);
        kernel_ticks++;
        tcp_tick();
        kernel_ticks++;
        tcp_tick();
        CHECK("and the rotation pumps it past the five held ones",
              n_segs == 1 && conns[0]->send_head == conns[0]->send_tail);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 11000, lost_from, lost_to, want, rto_before;
        uint16_t lport;
        static uint8_t bytes[24000];
        unsigned j, ticks = 0, rounds = 0, resent = 0, misplaced = 0;
        unsigned batches[8];

        reset_all();
        conn = open_conn(&sock, 83, &peer_seq, &lport);
        grow_window(&sock, conn, 83, lport, peer_seq, 20 * TCP_MSS);
        for (j = 0; j < sizeof(bytes); j++) {
            bytes[j] = (uint8_t)(j * 7 + 1);
        }
        rto_before = conn->rto;
        CHECK("a burst of 24000 bytes is taken", tcp_send(&sock, bytes, 24000) == 24000);
        CHECK("and goes out whole, seventeen segments",
              n_segs == 17 && conn->send_head == conn->send_tail);
        lost_from = conn->snd_una;
        lost_to = conn->snd_nxt;
        n_segs = 0;
        while (n_segs == 0 && ticks < 2 * TCP_RTO_MAX) {
            kernel_ticks++;
            tcp_tick();
            ticks++;
        }
        CHECK("the timer fired after the timeout", ticks == rto_before);
        CHECK("and sent one segment again", n_segs == 1);
        CHECK("the oldest unacknowledged one",
              n_segs == 1 && read32be(segs[0].data + 4) == lost_from);
        CHECK("with the window down to one segment", conn->cwnd == TCP_MSS);
        CHECK("and the timeout doubled", conn->rto == 2 * rto_before);
        CHECK("in the recovery that a timeout begins", conn->recovering == TCP_RECOVER_RTO);
        want = lost_from;
        while (conn->snd_una != lost_to && rounds < 8) {
            uint32_t acked_to = want;

            for (j = 0; j < n_segs && j < MAX_SEGS; j++) {
                uint32_t seq = read32be(segs[j].data + 4);
                unsigned b, plen = segs[j].len - TCP_HDR_SIZE;

                if (seq != want) {
                    misplaced++;
                }
                for (b = 0; b < plen && b + TCP_HDR_SIZE < sizeof(segs[j].data); b++) {
                    if (segs[j].data[TCP_HDR_SIZE + b] != bytes[seq - lost_from + b]) {
                        misplaced++;
                    }
                }
                want = seq + plen;
                acked_to = want;
                resent++;
            }
            batches[rounds] = n_segs;
            n_segs = 0;
            rx(83, lport, peer_seq, acked_to, F_ACK, 65535, (const char *)0, 0);
            rounds++;
        }
        CHECK("every acknowledgement sent the next segments at once, one more each time",
              rounds == 6 && batches[0] == 1 && batches[1] == 2 && batches[2] == 3 &&
                  batches[3] == 4 && batches[4] == 5 && batches[5] == 2);
        CHECK("the segments sent again were in order and carried the ring's bytes", misplaced == 0);
        CHECK("and every one was sent again once", resent == 17 && conn->retransmits == 17);
        CHECK("with no tick between the first and the last", ticks == rto_before);
        CHECK("the burst is acknowledged and the recovery is over",
              conn->snd_una == lost_to && conn->recovering == 0);
        CHECK("with the window grown a segment per acknowledgement, under the threshold",
              conn->cwnd == 7 * TCP_MSS && conn->cwnd <= conn->ssthresh);
        n_segs = 0;
        CHECK("new data is taken after the recovery", tcp_send(&sock, bytes, 100) == 100);
        CHECK("and goes out at once", n_segs == 1);
        kernel_ticks += 2;
        rx(83, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);
        CHECK("and its acknowledgement measures the timeout again", conn->rto == rto_before);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 12000, hole, hole_len, sent_end, ss;
        uint16_t lport;
        static uint8_t bytes[14600];

        reset_all();
        conn = open_conn(&sock, 84, &peer_seq, &lport);
        grow_window(&sock, conn, 84, lport, peer_seq, 12 * TCP_MSS);
        {
            unsigned b;

            for (b = 0; b < sizeof(bytes); b++) {
                bytes[b] = (uint8_t)(b * 5 + 3);
            }
        }
        CHECK("a burst of ten segments is taken",
              tcp_send(&sock, bytes, 14600) == 14600 && n_segs == 10);
        hole = read32be(segs[1].data + 4);
        hole_len = segs[1].len - TCP_HDR_SIZE;
        sent_end = conn->snd_nxt;
        rx(84, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        n_segs = 0;
        rx(84, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        rx(84, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        CHECK("two duplicates send nothing",
              n_segs == 0 && conn->recovering == 0 && conn->dup_acks == 2);
        rx(84, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        CHECK("the third sends the hole again at once",
              n_segs == 1 && read32be(segs[0].data + 4) == hole);
        CHECK("whole", n_segs == 1 && segs[0].len == TCP_HDR_SIZE + hole_len);
        {
            unsigned b, wrong = 0;

            for (b = 0; n_segs == 1 && b + TCP_HDR_SIZE < sizeof(segs[0].data); b++) {
                if (segs[0].data[TCP_HDR_SIZE + b] != bytes[TCP_MSS + b]) {
                    wrong++;
                }
            }
            CHECK("with the hole's own bytes", wrong == 0);
        }
        CHECK("beginning the recovery duplicates begin", conn->recovering == TCP_RECOVER_FAST);
        ss = (sent_end - hole) / 2u;
        CHECK("with the threshold at half the flight", conn->ssthresh == ss);
        CHECK("and the window at the threshold plus three segments",
              conn->cwnd == ss + 3 * TCP_MSS);
        rx(84, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        CHECK("a further duplicate adds a segment", conn->cwnd == ss + 4 * TCP_MSS);
        rx(84, lport, peer_seq, sent_end, F_ACK, 65535, (const char *)0, 0);
        CHECK("the acknowledgement of everything ends the recovery",
              conn->recovering == 0 && conn->snd_una == sent_end && conn->rto_timer == 0);
        CHECK("at the threshold", conn->cwnd == ss);
        CHECK("one segment was sent again", conn->retransmits == 1);
        n_segs = 0;
        rx(84, lport, peer_seq, sent_end, F_ACK, 65535, (const char *)0, 0);
        rx(84, lport, peer_seq, sent_end, F_ACK, 65535, (const char *)0, 0);
        rx(84, lport, peer_seq, sent_end, F_ACK, 65535, (const char *)0, 0);
        CHECK("duplicates of the acknowledgement that ended it begin no recovery",
              n_segs == 0 && conn->recovering == 0);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 13000, hole1, hole2, sent_end;
        uint16_t lport;
        static uint8_t bytes[14600];

        reset_all();
        conn = open_conn(&sock, 85, &peer_seq, &lport);
        grow_window(&sock, conn, 85, lport, peer_seq, 12 * TCP_MSS);
        CHECK("a burst of ten segments is taken",
              tcp_send(&sock, bytes, 14600) == 14600 && n_segs == 10);
        hole1 = read32be(segs[1].data + 4);
        hole2 = read32be(segs[4].data + 4);
        sent_end = conn->snd_nxt;
        rx(85, lport, peer_seq, hole1, F_ACK, 65535, (const char *)0, 0);
        n_segs = 0;
        rx(85, lport, peer_seq, hole1, F_ACK, 65535, (const char *)0, 0);
        rx(85, lport, peer_seq, hole1, F_ACK, 65535, (const char *)0, 0);
        rx(85, lport, peer_seq, hole1, F_ACK, 65535, (const char *)0, 0);
        CHECK("the third duplicate sends the first hole again",
              n_segs == 1 && read32be(segs[0].data + 4) == hole1);
        n_segs = 0;
        rx(85, lport, peer_seq, hole2, F_ACK, 65535, (const char *)0, 0);
        CHECK("the partial acknowledgement sends the second hole again at once",
              n_segs == 1 && read32be(segs[0].data + 4) == hole2);
        CHECK("in the same recovery",
              conn->recovering == TCP_RECOVER_FAST && conn->snd_una == hole2);
        rx(85, lport, peer_seq, sent_end, F_ACK, 65535, (const char *)0, 0);
        CHECK("and the acknowledgement of everything ends it",
              conn->recovering == 0 && conn->snd_una == sent_end && conn->retransmits == 2);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 14000, lost_from, lost_to;
        uint16_t lport;
        static uint8_t bytes[8760];
        unsigned ticks = 0;

        reset_all();
        conn = open_conn(&sock, 86, &peer_seq, &lport);
        grow_window(&sock, conn, 86, lport, peer_seq, 12 * TCP_MSS);
        CHECK("a burst of six segments is taken",
              tcp_send(&sock, bytes, 8760) == 8760 && n_segs == 6);
        lost_from = conn->snd_una;
        lost_to = conn->snd_nxt;
        n_segs = 0;
        tcp_close(&sock);
        CHECK("the close sends the FIN behind the burst",
              n_segs == 1 && (last_flags() & F_FIN) != 0 && conn->state == TCP_FIN_WAIT_1);
        n_segs = 0;
        while (n_segs == 0 && ticks < 2 * TCP_RTO_MAX) {
            kernel_ticks++;
            tcp_tick();
            ticks++;
        }
        CHECK("the timeout sends the oldest segment again, not the FIN",
              n_segs == 1 && read32be(segs[0].data + 4) == lost_from &&
                  (last_flags() & F_FIN) == 0);
        n_segs = 0;
        ip_tx_refuse = 1;
        rx(86, lport, peer_seq, lost_from + TCP_MSS, F_ACK, 65535, (const char *)0, 0);
        CHECK("its acknowledgement in FIN_WAIT_1 sends the next, which the driver refuses",
              n_segs == 0 && conn->tx_refused == 1 && conn->recovering == TCP_RECOVER_RTO);
        n_segs = 0;
        kernel_ticks++;
        tcp_tick();
        CHECK("and the tick sends the two the grown window allows",
              n_segs == 2 && read32be(segs[0].data + 4) == lost_from + TCP_MSS &&
                  read32be(segs[1].data + 4) == lost_from + 2 * TCP_MSS);
        n_segs = 0;
        rx(86, lport, peer_seq, lost_from + 3 * TCP_MSS, F_ACK, 65535, (const char *)0, 0);
        CHECK("their acknowledgement sends the three left, the window grown again",
              n_segs == 3 && read32be(segs[0].data + 4) == lost_from + 3 * TCP_MSS &&
                  read32be(segs[2].data + 4) == lost_from + 5 * TCP_MSS);
        rx(86, lport, peer_seq, lost_to, F_ACK, 65535, (const char *)0, 0);
        CHECK("the burst acknowledged ends the recovery, every segment sent again once",
              conn->recovering == 0 && conn->snd_una == lost_to && conn->retransmits == 6);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 16000, lost_from, lost_to;
        uint16_t lport;
        static uint8_t bytes[8760];
        unsigned ticks = 0;

        reset_all();
        conn = open_conn(&sock, 88, &peer_seq, &lport);
        grow_window(&sock, conn, 88, lport, peer_seq, 12 * TCP_MSS);
        CHECK("a burst of six segments is taken",
              tcp_send(&sock, bytes, 8760) == 8760 && n_segs == 6);
        lost_from = conn->snd_una;
        lost_to = conn->snd_nxt;
        n_segs = 0;
        while (n_segs == 0 && ticks < 2 * TCP_RTO_MAX) {
            kernel_ticks++;
            tcp_tick();
            ticks++;
        }
        CHECK("the timeout sends the oldest segment again", n_segs == 1);
        n_segs = 0;
        rx(88, lport, peer_seq, lost_from + TCP_MSS, F_ACK, 0, (const char *)0, 0);
        CHECK("its acknowledgement with a zero window sends nothing more",
              n_segs == 0 && conn->recovering == TCP_RECOVER_RTO);
        CHECK("and starts the probe timer", conn->zwp_timer != 0);
        while (n_segs == 0 && ticks < 4 * TCP_RTO_MAX) {
            kernel_ticks++;
            tcp_tick();
            ticks++;
        }
        CHECK("the probe goes out, one byte at the oldest unacknowledged",
              n_segs == 1 && segs[0].len == TCP_HDR_SIZE + 1 &&
                  read32be(segs[0].data + 4) == lost_from + TCP_MSS);
        n_segs = 0;
        rx(88, lport, peer_seq, lost_from + TCP_MSS, F_ACK, 65535, (const char *)0, 0);
        CHECK("the window's reopening resumes the recovery",
              n_segs >= 1 && conn->recovering == TCP_RECOVER_RTO);
        rx(88, lport, peer_seq, lost_from + TCP_MSS, F_RST, 0, (const char *)0, 0);
        CHECK("a reset ends the connection and its recovery",
              conn->state == TCP_CLOSED && conn->recovering == 0);
        n_segs = 0;
        kernel_ticks++;
        tcp_tick();
        kernel_ticks++;
        tcp_tick();
        CHECK("and the tick sends nothing for it", n_segs == 0);
        (void)lost_to;
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 17000, hole, sent_end;
        uint16_t lport;
        static uint8_t bytes[14600];

        reset_all();
        conn = open_conn(&sock, 89, &peer_seq, &lport);
        grow_window(&sock, conn, 89, lport, peer_seq, 12 * TCP_MSS);
        CHECK("a burst of ten segments is taken",
              tcp_send(&sock, bytes, 14600) == 14600 && n_segs == 10);
        hole = read32be(segs[1].data + 4);
        sent_end = conn->snd_nxt;
        rx(89, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        CHECK("the mark follows the acknowledgement", conn->recover == hole - 1);
        n_segs = 0;
        ip_tx_refuse = 1;
        rx(89, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        rx(89, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        rx(89, lport, peer_seq, hole, F_ACK, 65535, (const char *)0, 0);
        CHECK("the third duplicate's resend is refused and owed",
              n_segs == 0 && conn->fast_pending == 1 && conn->recovering == TCP_RECOVER_FAST);
        kernel_ticks++;
        tcp_tick();
        CHECK("the tick sends the hole",
              n_segs == 1 && read32be(segs[0].data + 4) == hole && conn->fast_pending == 0);
        rx(89, lport, peer_seq, sent_end, F_ACK, 65535, (const char *)0, 0);
        CHECK("and the acknowledgement of everything ends the recovery",
              conn->recovering == 0 && conn->snd_una == sent_end);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 18000, lost_from, lost_to;
        uint16_t lport;
        static uint8_t bytes[8760];
        unsigned ticks = 0;

        reset_all();
        conn = open_conn(&sock, 90, &peer_seq, &lport);
        grow_window(&sock, conn, 90, lport, peer_seq, 12 * TCP_MSS);
        CHECK("a burst of six segments is taken",
              tcp_send(&sock, bytes, 8760) == 8760 && n_segs == 6);
        lost_from = conn->snd_una;
        lost_to = conn->snd_nxt;
        rx(90, lport, peer_seq, lost_from, (uint8_t)(F_ACK | F_FIN), 65535, (const char *)0, 0);
        CHECK("the peer's FIN puts the connection in CLOSE_WAIT", conn->state == TCP_CLOSE_WAIT);
        peer_seq++;
        n_segs = 0;
        while (n_segs == 0 && ticks < 2 * TCP_RTO_MAX) {
            kernel_ticks++;
            tcp_tick();
            ticks++;
        }
        CHECK("the timeout sends the oldest segment again", n_segs == 1);
        n_segs = 0;
        rx(90, lport, peer_seq, lost_from + TCP_MSS, F_ACK, 65535, (const char *)0, 0);
        CHECK("its acknowledgement in CLOSE_WAIT sends two, the window grown",
              n_segs == 2 && conn->cwnd == 2 * TCP_MSS);
        rx(90, lport, peer_seq, lost_to, F_ACK, 65535, (const char *)0, 0);
        CHECK("and the burst acknowledged ends the recovery",
              conn->recovering == 0 && conn->snd_una == lost_to);
    }

    {
        struct socket sock;
        struct tcb *conn;
        uint32_t peer_seq = 15000, a_end, b_end;
        uint16_t lport;
        static uint8_t bytes[2920];

        reset_all();
        conn = open_conn(&sock, 87, &peer_seq, &lport);
        CHECK("two segments go out", tcp_send(&sock, bytes, 2920) == 2920 && n_segs == 2);
        a_end = read32be(segs[0].data + 4) + TCP_MSS;
        b_end = a_end + TCP_MSS;
        CHECK("the sample is on the first", conn->rtt_start != 0 && conn->rtt_seq == a_end);
        kernel_ticks++;
        rx(87, lport, peer_seq, a_end, F_ACK, 65535, (const char *)0, 0);
        CHECK("and its acknowledgement ends the sample", conn->rtt_start == 0);
        n_segs = 0;
        CHECK("a third segment goes out", tcp_send(&sock, bytes, 1460) == 1460 && n_segs == 1);
        CHECK("with a sample on it", conn->rtt_start != 0 && conn->rtt_seq == b_end + TCP_MSS);
        kernel_ticks++;
        rx(87, lport, peer_seq, b_end, F_ACK, 65535, (const char *)0, 0);
        CHECK("the acknowledgement of the second does not end it", conn->rtt_start != 0);
        kernel_ticks += 9;
        rx(87, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);
        CHECK("the acknowledgement of the third does", conn->rtt_start == 0);
    }

    {
        struct socket sock;
        struct tcb *conn;
        unsigned tries;

        reset_all();
        k_memset(&sock, 0, sizeof(sock));
        CHECK("a second connection can be opened", tcp_connect(&sock, PEER, 81) == 0);
        conn = sock.tcb;
        n_segs = 0;
        {
            unsigned last_at = 0, gaps[8], n_gaps = 0, before = 0;

            for (tries = 0; tries < 20000 && conn->state != TCP_CLOSED; tries++) {
                kernel_ticks++;
                tcp_tick();
                if (n_segs != before) {
                    if (last_at != 0 && n_gaps < 8) {
                        gaps[n_gaps++] = tries - last_at;
                    }
                    last_at = tries;
                    before = n_segs;
                }
            }
            CHECK("the connection was retransmitted more than once", n_gaps >= 2);
            CHECK("and each wait was longer than the one before",
                  n_gaps < 2 || (gaps[1] > gaps[0] && gaps[n_gaps - 1] > gaps[0]));
        }
        CHECK("and given up on", conn->state == TCP_CLOSED);
        CHECK("with each SYN sent again counted", conn->retransmits >= 2);
        CHECK("and the socket was told the attempt failed", cb_connected == 1);
        CHECK("and that the connection is over", cb_fin == 1);
        CHECK("with the error a caller reads back", conn->so_error != 0);
    }

    if (fails == 0) {
        printf("tcp_hosttest: PASS\n");
    }
    return fails > 255 ? 255 : fails;
}
