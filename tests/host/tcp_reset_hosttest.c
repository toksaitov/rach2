/*
 * tcp_reset_hosttest.c: the segments a host off the path can forge.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/network/tcp/rcvwnd.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/pressure_host.c
 *
 * A reset names a connection by its addresses and ports, which anyone can
 * write, and by a sequence number, which only the peer knows. So a reset is
 * taken only at the next sequence number this side expects; one elsewhere in
 * the receive window is answered with an acknowledgement, which the true
 * peer answers with a reset at that number; one outside the window is
 * dropped (RFC 5961, section 3.2). While the connection is still opening
 * there is no window, and a reset counts only when it acknowledges the SYN
 * (RFC 793). A host that could predict this side's initial sequence number
 * could forge the rest of a handshake, so the number is a clock plus a keyed
 * hash of the two addresses and two ports (RFC 6528). And a handshake that
 * completes after its listener closed has nobody to accept it, so it is
 * answered with a reset and freed rather than left open for ever.
 *
 * `tcp.c` is included, as in `tcp_hosttest.c`, with the layer under it
 * replaced by a recorder of every segment `ip_tx` would have sent and the
 * layer above by one that records every callback. The key comes from a
 * stand-in for `krandom_u32` whose four words are the bytes 00 to 0f, the
 * key of SipHash's own test vectors, and the addresses and ports make the
 * twelve bytes 00 to 0b, so the hash of every connection here is a constant
 * the test states.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"
#include "../../rach2/sys/network/tcp/tcp.h"
#include "../../rach2/sys/network/driver/netdev.h"
#include "../../rach2/sys/network/socket/socket.h"

/* ---- the clock, the key, and the layers above and below ---- */

volatile uint32_t kernel_ticks;

/* This machine and its peer, as little-endian words of the bytes 00 to 07. */
#define SELF      0x03020100u
#define PEER      0x07060504u
/* The two ports, as little-endian halves of the bytes 08 to 0b. */
#define SELF_PORT 0x0908u
#define PEER_PORT 0x0b0au

/* SipHash-2-4 of the twelve bytes 00 to 0b under the key 00 to 0f, the key
 * and message form of SipHash's test vectors, low 32 bits. */
#define SIP_LOW32 0x860ee5fbu

uint32_t net_ip_addr = SELF;

static unsigned key_draws;

/* The key words in the order `tcp_isn_key` stores them, whenever it asks. */
uint32_t krandom_u32(void)
{
    return 0x03020100u + 0x04040404u * (key_draws++ % 4);
}

#define MAX_SEGS 16

static struct {
    uint32_t seq, ack;
    uint8_t flags;
} segs[MAX_SEGS];
static unsigned n_segs;
static unsigned cb_connected, cb_fin, cb_accept, unrefs;

uint32_t read32be(const uint8_t *p);

int ip_tx(uint32_t dst, uint8_t proto, const uint8_t *payload, uint16_t len, uint8_t ttl)
{
    (void)dst;
    (void)proto;
    (void)len;
    (void)ttl;
    if (n_segs < MAX_SEGS) {
        segs[n_segs].seq = read32be(payload + 4);
        segs[n_segs].ack = read32be(payload + 8);
        segs[n_segs].flags = payload[13];
    }
    n_segs++;
    return 0;
}

void socket_tcp_connected(struct socket *sock)
{
    (void)sock;
    cb_connected++;
}

void socket_tcp_fin(struct socket *sock)
{
    (void)sock;
    cb_fin++;
}

void socket_tcp_writable(struct socket *sock)
{
    (void)sock;
}

uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len)
{
    (void)sock;
    (void)data;
    return len;
}

uint16_t socket_tcp_hold(struct socket *sock, uint32_t offset, const uint8_t *data, uint16_t len)
{
    (void)sock;
    (void)offset;
    (void)data;
    return len;
}

void socket_tcp_release(struct socket *sock, uint32_t n)
{
    (void)sock;
    (void)n;
}

void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock)
{
    (void)listen_sock;
    (void)conn_sock;
    cb_accept++;
}

/* The sockets a passive open asks for, from a pool the cases reuse, so that
 * nothing here is left for the leak checker. */
static struct socket pool[2];
static unsigned pooled;

struct socket *socket_alloc(int type)
{
    struct socket *s;

    if (pooled >= sizeof(pool) / sizeof(pool[0])) {
        return 0;
    }
    s = &pool[pooled++];
    k_memset(s, 0, sizeof(*s));
    s->type = type;
    s->refcount = 1;
    return s;
}

void socket_unref(struct socket *sock)
{
    (void)sock;
    unrefs++;
}

uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void write16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

void write32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The IP header checksum, which `tcp_checksum` reuses for the pseudo-header. */
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

uint32_t net_lock(void)
{
    return 0;
}

void net_unlock(uint32_t flags)
{
    (void)flags;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* One interface, holding this machine's address. */
static struct netdev the_iface;

struct netdev *netdev_route(uint32_t dst, uint32_t *via)
{
    *via = dst;
    the_iface.ip = net_ip_addr;
    return &the_iface;
}

int netdev_owns(uint32_t ip)
{
    return ip != 0 && ip == net_ip_addr;
}

#include "../../rach2/sys/network/tcp/tcp.c"

/* ---- the cases ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

#define F_SYN 0x02
#define F_RST 0x04
#define F_ACK 0x10

/** Free every block, forget every segment and callback, and set the clock. */
static void reset_all(void)
{
    while (tcbs) {
        tcb_free(tcbs);
    }
    n_segs = 0;
    cb_connected = cb_fin = cb_accept = unrefs = 0;
    pooled = 0;
    kernel_ticks = 1000;
}

/** One segment from the peer's port to `dport`, checksummed as it is sent. */
static void rx(uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags)
{
    uint8_t seg[20];

    write16be(seg + 0, PEER_PORT);
    write16be(seg + 2, dport);
    write32be(seg + 4, seq);
    write32be(seg + 8, ack);
    seg[12] = 0x50;
    seg[13] = flags;
    write16be(seg + 14, 65535);
    write16be(seg + 16, 0);
    write16be(seg + 18, 0);
    write16be(seg + 16, tcp_checksum(PEER, net_ip_addr, seg, 20));
    tcp_rx(PEER, net_ip_addr, seg, 20);
}

/** Open a connection from SELF_PORT to the peer and take its SYN-ACK. */
static struct tcb *open_conn(struct socket *sock, uint32_t peer_seq)
{
    k_memset(sock, 0, sizeof(*sock));
    sock->type = SOCK_STREAM;
    sock->local_port = SELF_PORT;
    n_segs = 0;
    CHECK("a connection can be opened", tcp_connect(sock, PEER, PEER_PORT) == 0);
    rx(SELF_PORT, peer_seq, segs[0].seq + 1, (uint8_t)(F_SYN | F_ACK));
    n_segs = 0;
    return sock->tcb;
}

/**
 * A reset at the next expected sequence number closes the connection; one
 * elsewhere in the window draws an acknowledgement that carries the numbers
 * the true peer must answer with, and closes nothing; one outside the window,
 * below it or at its end, is dropped without an answer.
 */
static void test_established(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt;

    reset_all();
    conn = open_conn(&sock, 5000);
    CHECK("the connection is established", conn && conn->state == TCP_ESTABLISHED);
    nxt = conn->rcv_nxt;
    CHECK("the receive window is open", conn->rcv_wnd > 1000);

    rx(SELF_PORT, nxt + 1000, 0, F_RST);
    CHECK("a reset inside the window but not at the next sequence number closes nothing",
          conn->state == TCP_ESTABLISHED && conn->so_error == 0 && cb_fin == 0);
    CHECK("and draws one acknowledgement at the numbers the peer must answer",
          n_segs == 1 && segs[0].flags == F_ACK && segs[0].seq == conn->snd_nxt &&
              segs[0].ack == nxt);

    n_segs = 0;
    rx(SELF_PORT, nxt - 1, 0, F_RST);
    rx(SELF_PORT, nxt + conn->rcv_wnd, 0, F_RST);
    CHECK("a reset outside the window is dropped without an answer",
          n_segs == 0 && conn->state == TCP_ESTABLISHED && conn->so_error == 0);

    rx(SELF_PORT, nxt, 0, F_RST);
    CHECK("a reset at the next sequence number closes the connection",
          conn->state == TCP_CLOSED && conn->so_error == ECONNRESET && cb_fin == 1 && n_segs == 0);
    tcp_close(&sock);
    reset_all();
}

/**
 * While the connection is opening, a reset counts only when it acknowledges
 * the SYN. One without the ACK flag leaves the open as it was, even when its
 * acknowledgement field has the number that acknowledges the SYN, and so
 * does one that acknowledges another number.
 */
static void test_syn_sent(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t iss;

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("an open can be started", tcp_connect(&sock, PEER, PEER_PORT) == 0);
    conn = sock.tcb;
    iss = segs[0].seq;
    n_segs = 0;

    rx(SELF_PORT, 777, iss + 1, F_RST);
    CHECK("a reset with the acknowledgement of the SYN but no ACK flag leaves the open",
          conn->state == TCP_SYN_SENT && conn->so_error == 0 && n_segs == 0);
    rx(SELF_PORT, 777, iss, (uint8_t)(F_RST | F_ACK));
    CHECK("a reset that acknowledges another number leaves the open",
          conn->state == TCP_SYN_SENT && conn->so_error == 0 && n_segs == 0);

    rx(SELF_PORT, 0, iss + 1, (uint8_t)(F_RST | F_ACK));
    CHECK("a reset that acknowledges the SYN refuses the connection",
          conn->state == TCP_CLOSED && conn->so_error == ECONNREFUSED);
    tcp_close(&sock);
    reset_all();
}

/**
 * A handshake completed while its listener is open goes on the listener's
 * queue. Closing the listener frees the handshakes still in progress on its
 * port with their sockets, so one completed after the close is answered with
 * a reset at the number the peer acknowledged, and a listener opened on the
 * port since does not receive it.
 */
static void test_listener_gone(void)
{
    struct socket lsock;
    struct socket later;
    uint32_t iss;
    int closed;

    for (closed = 0; closed <= 2; closed++) {
        reset_all();
        k_memset(&lsock, 0, sizeof(lsock));
        lsock.type = SOCK_STREAM;
        CHECK("a listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
        rx(SELF_PORT, 9000, 0, F_SYN);
        CHECK("a SYN to the listener is answered with a SYN-ACK",
              n_segs == 1 && segs[0].flags == (F_SYN | F_ACK) && segs[0].ack == 9001);
        iss = segs[0].seq;
        if (closed) {
            tcp_close(&lsock);
            CHECK("closing the listener frees the handshake in progress and its socket",
                  tcbs == 0 && unrefs == 1);
        }
        if (closed == 2) {
            k_memset(&later, 0, sizeof(later));
            later.type = SOCK_STREAM;
            CHECK("a listener can be opened on the port again", tcp_listen(&later, SELF_PORT) == 0);
        }
        n_segs = 0;
        rx(SELF_PORT, 9001, iss + 1, F_ACK);
        if (!closed) {
            CHECK("a handshake completed beside its listener is queued for accept",
                  cb_accept == 1 && n_segs == 0);
            tcp_close(&lsock);
            continue;
        }
        if (closed == 1) {
            CHECK("a handshake completed after its listener closed is answered with a reset",
                  n_segs == 1 && (segs[0].flags & F_RST) && segs[0].seq == iss + 1);
            CHECK("and nobody accepts it", tcbs == 0 && cb_accept == 0);
            continue;
        }
        CHECK("a listener opened after the close does not receive the old handshake",
              cb_accept == 0 && tcbs != 0 && tcbs->next == 0 && tcbs->state == TCP_LISTEN);
        tcp_close(&later);
    }
    reset_all();
}

/**
 * The key is drawn at the first open after `tcp_init` and not again until the
 * next `tcp_init`. An active and a passive open on the same addresses and
 * ports start at the clock, 2500 to a tick, plus the keyed hash of those
 * twelve bytes.
 */
static void test_isn(void)
{
    struct socket sock, lsock, other;
    uint32_t want;

    reset_all();
    tcp_init();
    key_draws = 0;
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    kernel_ticks = 123456;
    want = 123456u * 2500u + SIP_LOW32;
    CHECK("an open can be started", tcp_connect(&sock, PEER, PEER_PORT) == 0);
    CHECK("the first open draws the four words of the key", key_draws == 4);
    CHECK("an active open starts at the clock plus the keyed hash of its addresses and ports",
          n_segs == 1 && segs[0].seq == want && sock.tcb && sock.tcb->iss == want);

    k_memset(&other, 0, sizeof(other));
    other.type = SOCK_STREAM;
    CHECK("a second open can be started", tcp_connect(&other, PEER, 80) == 0);
    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("a listener can be opened", tcp_listen(&lsock, 81) == 0);
    CHECK("later opens and listeners draw no key", key_draws == 4);
    tcp_close(&sock);
    tcp_close(&other);
    tcp_close(&lsock);

    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("the listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
    kernel_ticks = 7;
    n_segs = 0;
    rx(SELF_PORT, 4242, 0, F_SYN);
    CHECK("a passive open starts at the clock plus the same keyed hash",
          n_segs == 1 && segs[0].seq == 7u * 2500u + SIP_LOW32);
    tcp_close(&lsock);

    reset_all();
    tcp_init();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    CHECK("an open after tcp_init can be started", tcp_connect(&sock, PEER, PEER_PORT) == 0);
    CHECK("tcp_init makes the next open draw a new key", key_draws == 8);
    tcp_close(&sock);
    reset_all();
}

int main(void)
{
    test_established();
    test_syn_sent();
    test_listener_gone();
    test_isn();
    if (fails == 0) {
        printf("tcp_reset_hosttest: PASS\n");
    }
    return fails != 0;
}
