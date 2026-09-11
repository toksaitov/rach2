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
 * listens on, and the control-block pool running out. Those are the cases
 * here, driven directly against `tcp.c` with the layer under it replaced by a
 * recorder that keeps every segment `ip_tx` would have sent, and the layer
 * above it by one that records every callback.
 *
 * The clock is a variable, so a retransmission that takes six seconds of a
 * real machine's time takes none here.
 *
 * The file is included, not linked, so each case can put the table back to its
 * initial state; the kernel has one boot per run and needs no such thing.
 */

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long n);

/* The kernel headers define the fixed-width types themselves. */
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/network/tcp/tcp.h"
#include "../../rach2/sys/network/driver/netdev.h"

/* ---- the clock, and the layers above and below ---- */

volatile uint32_t kernel_ticks;

#define MAX_SEGS 64

static struct {
    uint32_t dst;
    uint8_t proto;
    uint16_t len;
    uint8_t data[128];
} segs[MAX_SEGS];
static unsigned n_segs;

static unsigned cb_connected, cb_fin, cb_writable, cb_deliver, cb_accept;
static unsigned delivered_bytes;

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
static uint32_t stream_base;
static int stream_on;
uint32_t read32be(const uint8_t *p);

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
    cb_writable++;
}

uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len)
{
    (void)sock;
    (void)data;
    cb_deliver++;
    delivered_bytes += len;
    return len;
}

void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock)
{
    (void)listen_sock;
    (void)conn_sock;
    cb_accept++;
}

void socket_unref(struct socket *sock)
{
    (void)sock;
}

struct socket *socket_alloc(int type)
{
    (void)type;
    return (struct socket *)0;
}

uint32_t net_ip_addr = 0x0A00020Fu; /* 10.0.2.15 */

/* The four byte-order helpers, written out rather than dragging in the boot
   report that shares their file. */
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

/* The IP header checksum, which `tcp_checksum` reuses for the pseudo-header.
   Three lines, and its own file drags the whole IP layer in. */
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

/* The interface layer, as this test presents it: one interface holding the
 * machine's address, which is what every case here is written against. */
static struct netdev the_iface;

struct netdev *netdev_route(uint32_t dst, uint32_t *via)
{
    (void)dst;
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

#define PEER      0x0A000202u /* 10.0.2.2 */
#define TCP_PROTO 6

/* The flags in a segment this test builds or reads. */
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

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

/* One segment from the peer, with the header this stack reads. */
static void rx(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
               uint16_t window, const char *payload, uint16_t plen)
{
    uint8_t seg[128];
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
    for (i = 0; i < plen; i++) {
        seg[20 + i] = (uint8_t)payload[i];
    }
    /* The receiver verifies this, so the test computes it the same way the
       sender does: a segment with a wrong checksum is dropped, and every case
       below would then be checking that. */
    write16be(seg + 16, tcp_checksum(PEER, net_ip_addr, seg, (uint16_t)(20 + plen)));
    tcp_rx(PEER, net_ip_addr, seg, (uint16_t)(20 + plen));
}

/* The flags of the segment this stack sent last. */
static uint8_t last_flags(void)
{
    if (n_segs == 0 || n_segs > MAX_SEGS) {
        return 0;
    }
    return segs[n_segs - 1].data[13];
}

int main(void)
{
    struct tcb *t;
    unsigned i;

    /* The blocks, past the sixteen the pool once held: forty at once, one
       freed from the middle of the list and the rest still there. */
    tcp_init();
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

    /* A segment for a port nobody listens on is answered with a reset, which
       is what tells the peer to stop retransmitting. */
    reset_all();
    rx(4000, 9999, 100, 0, F_SYN, 4096, (const char *)0, 0);
    CHECK("a SYN for a closed port is answered", n_segs == 1);
    CHECK("and the answer is a reset", (last_flags() & F_RST) != 0);

    /* A reset for a port nobody listens on is not answered, or two machines
       would reset each other for ever. */
    reset_all();
    rx(4000, 9999, 100, 0, F_RST, 0, (const char *)0, 0);
    CHECK("a reset for a closed port is not answered", n_segs == 0);

    /* A connection, opened and carried. Everything below is measured against
       the sequence numbers this handshake settles. */
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

        /* The peer answers. The connection opens and the socket is told. */
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_SYN | F_ACK), 4096, (const char *)0,
           0);
        CHECK("the handshake completes", cb_connected == 1);
        CHECK("and the connection is established", conn->state == TCP_ESTABLISHED);
        CHECK("and is acknowledged", (last_flags() & F_ACK) != 0);
        peer_seq++;

        /* Data in order is delivered once. */
        n_segs = 0;
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "abcd", 4);
        CHECK("data in order is delivered", cb_deliver == 1 && delivered_bytes == 4);
        CHECK("and acknowledged", n_segs >= 1 && (last_flags() & F_ACK) != 0);
        CHECK("offering the ring clamped to the field, not zero",
              n_segs >= 1 && read16be(segs[n_segs - 1].data + 14) == 65535);

        /* The same segment again is not delivered twice, and is acknowledged
           again so a peer that lost the first acknowledgement stops asking. */
        cb_deliver = 0;
        delivered_bytes = 0;
        n_segs = 0;
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "abcd", 4);
        CHECK("the same data again is not delivered twice", delivered_bytes == 0);
        CHECK("and is acknowledged again", n_segs >= 1);
        peer_seq += 4;

        /* A segment past the hole is not delivered, and the acknowledgement
           still names the byte the receiver is waiting for. */
        cb_deliver = 0;
        delivered_bytes = 0;
        n_segs = 0;
        rx((uint16_t)80, lport, peer_seq + 4, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "ijkl", 4);
        CHECK("a segment past a hole is not delivered", delivered_bytes == 0);
        CHECK("and the acknowledgement still asks for the hole",
              n_segs >= 1 && read32be(segs[n_segs - 1].data + 8) == peer_seq);

        /* The hole filled: that segment is delivered. The one beyond it is
           not held by this stack, so the peer sends it again. */
        cb_deliver = 0;
        delivered_bytes = 0;
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_PSH), 4096, "efgh", 4);
        CHECK("the segment that fills the hole is delivered", delivered_bytes == 4);
        peer_seq += 4;

        /* A window of zero stops the sender: data offered while it is closed
           must not go out, and must go out when it reopens. Checking the
           stored window alone would pass a stack that ignored it. */
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

        /* The peer's FIN closes the reading half and is acknowledged. */
        n_segs = 0;
        rx((uint16_t)80, lport, peer_seq, iss + 1, (uint8_t)(F_ACK | F_FIN), 4096, (const char *)0,
           0);
        CHECK("a FIN is reported to the socket", cb_fin == 1);
        CHECK("and acknowledged", n_segs >= 1 && (last_flags() & F_ACK) != 0);
    }

    /* A ring's worth of data: the ring takes it in one call, the bursts are
       bounded by the windows, every byte leaves in segments of at most one
       MSS, and a transmit the driver refuses stays in the ring for the next
       pump rather than being counted as sent. */
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

        /* A second transfer takes the ring's indexes past 65536, and every
           segment recorded carries the byte the stream has at its sequence
           number, so a wrap that copied from the wrong place would show. */
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

        /* A refusal with a segment already out: the first goes, the rest
           waits, and the acknowledgement of the first pumps the rest. */
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

        /* A permanent error is not a full ring: the segment counts as sent
           and the retransmission timer, not the tick's pump, owns it. */
        n_segs = 0;
        ip_tx_refuse = 1;
        ip_tx_refuse_err = -ENETUNREACH;
        CHECK("data is taken under a permanent error", tcp_send(&sock, big, 100) == 100);
        CHECK("and counted as sent, for the retransmission timer",
              conn->send_head == conn->send_tail && conn->rto_timer != 0);
        ip_tx_refuse_err = -ENOBUFS;
        rx((uint16_t)82, lport, peer_seq, conn->snd_nxt, F_ACK, 65535, (const char *)0, 0);

        /* A driver that refuses for good: the bytes wait, and the connection
           is given up after the bound rather than kept for ever. */
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

    /* A zero window that closes over a refused segment is flow control, not
       a continuing refusal: the connection is kept through the bound and the
       segment goes when the window reopens. And a connection last in the
       list behind four that a zero window holds is still pumped, since the
       tick's pumps rotate. The list is newest first, so the first opened is
       the last pumped. */
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

    /* A peer that stops answering: the stack retransmits with a doubling
       timer and gives up, rather than holding the connection for ever. */
    {
        struct socket sock;
        struct tcb *conn;
        unsigned tries;

        reset_all();
        k_memset(&sock, 0, sizeof(sock));
        CHECK("a second connection can be opened", tcp_connect(&sock, PEER, 81) == 0);
        conn = sock.tcb;
        n_segs = 0;
        /* The retransmission timer counts ticks and `tcp_tick` is one tick,
           so this is the timer running rather than the clock jumping: the
           five backoffs together are about six thousand of them. The gaps
           between the attempts are recorded, because a stack that retried
           every tick would also eventually give up and pass a check that only
           counted the attempts. */
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
        /* The block stays until the socket lets go of it, which is what a
           program still holding the descriptor expects; what the socket is
           told is that the connection ended. */
        CHECK("and the socket was told the attempt failed", cb_connected == 1);
        CHECK("and that the connection is over", cb_fin == 1);
        CHECK("with the error a caller reads back", conn->so_error != 0);
    }

    if (fails == 0) {
        printf("tcp_hosttest: PASS\n");
    }
    return fails;
}
