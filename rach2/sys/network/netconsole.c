/*
 * netconsole.c: the kernel log, repeated onto the wire as UDP datagrams to
 * the targets of `netconsole=` and as broadcast frames, as netconsole.h
 * describes it.
 *
 * The output path, `netconsole_putc` and the flush it starts, runs inside
 * `klog`'s delivery, which is entered from anywhere in the kernel, interrupt
 * handlers and the panic path included, so it does three things and no
 * others: it appends a character to a fixed ring, it fills a fixed frame
 * buffer, and it calls the Ethernet layer. It allocates nothing and waits for
 * nothing. Only `netconsole_setup`, at boot, allocates the targets.
 *
 * The ring keeps what the log writes before a card is up: the memory layout,
 * the module ranges, the device probes, which the first frames carry. A card
 * whose link is not up yet, as a real card's is not for the seconds its PHY
 * takes to negotiate, could not send them, so they wait in the ring until
 * the link is up and the next line starts a drain, even a line the full ring
 * has no room for. A frame the card refuses, because the link went down in
 * the middle of a drain or its transmit ring is full, stays in the ring too,
 * and the drain stops until the next line: characters leave the ring only
 * once their frame is sent, which also keeps a character that arrives during
 * the send from taking their place. While a drain is held so, only a newline
 * starts another, and not the 512 waiting characters that otherwise do
 * (`nc_held`), so the card is asked once a line.
 *
 * Two things can interleave here, and each has its own guard.
 *
 * Interrupts. A system call logs with interrupts on, and the timer and the
 * cards log from their handlers, so the ring and the Ethernet layer's frame
 * buffer can each be entered from two contexts. net.h gives the rule for the
 * frame buffer, and the ring needs the same: without it, a handler that logs
 * between a writer computing its next index and storing it writes into the
 * slot the writer then overwrites, and a character is lost from the log with
 * nothing to show for it. So `net_lock` brackets the ring's update and each
 * frame; each frame and not the whole drain, since a full ring is sixteen
 * frames and the timer's interrupt should not wait for all of them.
 *
 * Recursion. A driver reports a transmit error with a record of its own,
 * which comes back here in the same context, where disabling interrupts
 * protects nothing. A record raised during `klog`'s delivery waits in `klog`'s
 * queue until that delivery is done, so it cannot. One raised inside the
 * flush `netconsole_enable` starts can, since that flush is no delivery.
 * `nc_sending` stops it: while a frame is being sent, a character only joins
 * the ring. The characters it adds would still extend the drain that is
 * running, so a drain fixes its end at the ring's head when it starts. A card
 * that refuses every frame costs one refused frame a line, and the ring fills
 * and drops the newest characters, as it does with no link.
 *
 * The targets. Each keeps its own place in the ring, so one whose link is down
 * or that has no source address yet holds only its own records back, and the
 * ring frees a character once the frames and every target have sent it:
 * `nc_tail` is the frames' place, or, without them, the place of the target
 * furthest behind. A target is sent whole lines, which are whole records,
 * since a record is one line: one datagram a record, as Linux's `write_msg`
 * sends one a message (drivers/net/netconsole.c). The datagram is built as
 * Linux's `netpoll_send_udp` builds it (net/core/netpoll.c): IPv4 with TTL 64
 * and an identification that counts, or IPv6 with hop limit 32, and a UDP
 * checksum over the pseudo-header, 0xffff in place of 0.
 */

#include "../klog.h"
#include "netconsole.h"
#include "net.h"
#include "core/byteorder.h"
#include "ethernet/ethernet.h"
#include "driver/netdev.h"
#include "ip/ip.h"
#include "../lib/krandom.h"
#include "../mm/kheap.h"
#include "../syscall.h"
#include "ip6/ip6.h"

/* The characters not yet sent, a boot log's worth ahead of the probe. One
 * place is kept empty to tell a full ring from an empty one. */
#define NETCONSOLE_RING 8192

static char nc_ring[NETCONSOLE_RING];
static uint32_t nc_head; /* where the next character goes */
static uint32_t nc_tail; /* the oldest character not yet sent */

/* The frame payload being built: the header, then the characters. */
static uint8_t nc_payload[NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX];

static uint32_t nc_boot_id;
static uint32_t nc_seq;     /* the next frame's sequence number */
static uint32_t nc_frames;  /* frames the card took */
static uint32_t nc_dropped; /* characters the ring had no room for */
static int nc_enabled;
static int nc_sending; /* a drain runs: a character only joins the ring */
static int nc_held;    /* the last drain stopped: no link or a refused frame */

static const uint8_t nc_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* The longest piece of a line one extended datagram carries before escaping:
 * a whole record, KLOG_RECORD_MAX. */
#define NC_EXT_LINE KLOG_RECORD_MAX

/* An extended message whole: the release and its comma, the header, and the
 * line with each byte escaped to at most four. */
#define NC_EXT_MAX (sizeof(UTS_RELEASE) + 64 + 4 * NC_EXT_LINE + 1)

/* The IPv4 and IPv6 headers, and the UDP header after either. */
#define NC_IP4_HDR 20
#define NC_IP6_HDR 40
#define NC_UDP_HDR 8

/**
 * One target of `netconsole=`, as Linux's `struct netpoll` and the two flags
 * of `struct netconsole_target` hold one. Addresses are in network order, an
 * IPv4 address in the first four bytes.
 */
struct nc_target {
    struct nc_target *next;
    struct netdev *nd; /* the interface `dev` named, found at setup */
    int ipv6;
    int extended; /* `+`: Linux's extended form */
    int release;  /* `r`: the release before it */
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t local_ip[16]; /* all zero: the interface's, at each datagram */
    uint8_t remote_ip[16];
    uint8_t remote_mac[6];
    uint32_t tail;      /* the oldest character not yet sent to it */
    int held;           /* its last drain stopped: no link, no source, refused */
    uint32_t datagrams; /* datagrams the card took */
    uint32_t seq;       /* the next extended record's sequence number */
    int level;          /* the last record's level, for a piece of a longer line */
    char usec[24];      /* and its stamp in microseconds, as digits */
};

static struct nc_target *nc_targets;
static int nc_setup_done;
static uint16_t nc_ip_id; /* the last IPv4 identification sent */

/* The line being sent, its extended form, and the datagram, IPv6 header the
 * largest, for which ethernet_tx adds the Ethernet header. */
static char nc_line[NETCONSOLE_CHUNK_MAX];
static char nc_ext[NC_EXT_MAX];
static uint8_t nc_pkt[NC_IP6_HDR + NC_UDP_HDR + NETCONSOLE_CHUNK_MAX];

/** How many characters wait behind the place `tail`. */
static uint32_t nc_behind(uint32_t tail)
{
    return (nc_head >= tail) ? (nc_head - tail) : (NETCONSOLE_RING - tail + nc_head);
}

/** How many characters are waiting for the frames, or, without them, for the
 * target furthest behind. */
static uint32_t nc_pending(void)
{
    return nc_behind(nc_tail);
}

/** How many characters the ring holds: what the sender furthest behind has
 * still to send. */
static uint32_t nc_used(void)
{
    uint32_t most = nc_pending();
    struct nc_target *t;

    for (t = nc_targets; t; t = t->next) {
        if (nc_behind(t->tail) > most) {
            most = nc_behind(t->tail);
        }
    }
    return most;
}

/** A full ring drops the newest character, since the oldest are the boot
 * log; a newline still starts a drain, which makes room. */
void netconsole_putc(char c)
{
    uint32_t flags = net_lock();
    uint32_t next = nc_head + 1;
    int send;

    if (next == NETCONSOLE_RING) {
        next = 0;
    }
    if (nc_used() == NETCONSOLE_RING - 1) {
        nc_dropped++;
        send = (nc_enabled || nc_targets) && !nc_sending && c == '\n';
        net_unlock(flags);
        if (send) {
            netconsole_flush();
        }
        return;
    }
    nc_ring[nc_head] = c;
    nc_head = next;

    send = (nc_enabled || nc_targets) && !nc_sending &&
           (c == '\n' || (nc_enabled && !nc_held && nc_pending() >= NETCONSOLE_PAYLOAD_MAX));
    net_unlock(flags);

    if (send) {
        netconsole_flush();
    }
}

/** Send the ring to the first interface in broadcast frames, as
 * `netconsole_flush` says. */
static void nc_flush_frames(void)
{
    uint32_t flags = net_lock();
    uint32_t end;

    if (!nc_enabled || nc_sending) {
        net_unlock(flags);
        return;
    }
    if (!netdev_running(netdev_first())) {
        nc_held = 1;
        net_unlock(flags);
        return;
    }
    nc_sending = 1;
    end = nc_head;
    net_unlock(flags);

    for (;;) {
        uint16_t n = 0;
        uint32_t at;

        flags = net_lock();
        at = nc_tail;
        while (at != end && n < NETCONSOLE_PAYLOAD_MAX) {
            nc_payload[NETCONSOLE_HDR_LEN + n] = (uint8_t)nc_ring[at];
            n++;
            at++;
            if (at == NETCONSOLE_RING) {
                at = 0;
            }
        }
        if (n == 0) {
            nc_sending = 0;
            net_unlock(flags);
            return;
        }

        nc_payload[0] = 'R';
        nc_payload[1] = 'C';
        nc_payload[2] = 'H';
        nc_payload[3] = '2';
        write32be(nc_payload + 4, nc_boot_id);
        write32be(nc_payload + 8, nc_seq);
        write16be(nc_payload + 12, n);
        write16be(nc_payload + 14, 0);
        write16be(nc_payload + 14, ip_checksum(nc_payload, (uint16_t)(NETCONSOLE_HDR_LEN + n)));

        if (ethernet_tx(netdev_first(), nc_broadcast, NETCONSOLE_ETHERTYPE, nc_payload,
                        (uint16_t)(NETCONSOLE_HDR_LEN + n)) != NETDEV_TX_OK) {
            nc_held = 1;
            nc_sending = 0;
            net_unlock(flags);
            return;
        }
        nc_held = 0;
        nc_tail = at;
        nc_seq++;
        nc_frames++;
        net_unlock(flags);
    }
}

/* ---- the targets ---- */

/** Add the `len` bytes at `p` to the one's complement sum `sum`, as 16-bit
 * big-endian words, the last byte of an odd length padded with zero. */
static uint32_t nc_sum(uint32_t sum, const uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    if (len & 1u) {
        sum += (uint32_t)p[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return sum;
}

/** Whether the `n` bytes at `p` are all zero. */
static int nc_zero(const uint8_t *p, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/** Whether `a` is an IPv6 link-local address, fe80::/10. */
static int nc_linklocal(const uint8_t *a)
{
    return a[0] == 0xFE && (a[1] & 0xC0) == 0x80;
}

/**
 * Write the address `t` sends from to `src`: the one it gave, or the
 * interface's first IPv4 address, or for IPv6 its first address that is not
 * tentative and whose link-local scope matches the target's, as Linux's
 * `netpoll_take_ipv6` chooses (net/core/netpoll.c). Returns 0 while there is
 * none. Under `net_lock`.
 */
static int nc_source(const struct nc_target *t, uint8_t *src)
{
    int i;

    if (!nc_zero(t->local_ip, t->ipv6 ? 16 : 4)) {
        for (i = 0; i < 16; i++) {
            src[i] = t->local_ip[i];
        }
        return 1;
    }
    if (!t->ipv6) {
        if (t->nd->ip == 0) {
            return 0;
        }
        write32be(src, t->nd->ip);
        return 1;
    }
    for (i = 0; i < t->nd->naddr6; i++) {
        const struct netdev_addr6 *a = &t->nd->addr6[i];
        int j;

        if ((a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED)) != 0 ||
            nc_linklocal(a->addr) != nc_linklocal(t->remote_ip)) {
            continue;
        }
        for (j = 0; j < 16; j++) {
            src[j] = a->addr[j];
        }
        return 1;
    }
    return 0;
}

/**
 * Copy into `nc_line` the line that starts at the place `from`, its newline
 * included, and return its length: at most `limit` characters, a longer line
 * going in pieces of `limit`. Returns 0 when the line before `end` is not
 * whole and shorter than `limit`, since its newline is still to come.
 */
static uint32_t nc_take(uint32_t from, uint32_t end, uint32_t limit)
{
    uint32_t n = 0;

    while (from != end && n < limit) {
        char c = nc_ring[from];

        nc_line[n++] = c;
        from++;
        if (from == NETCONSOLE_RING) {
            from = 0;
        }
        if (c == '\n') {
            return n;
        }
    }
    return n == limit ? n : 0;
}

/** Write `v` in decimal at `out` and return the digits written. */
static uint32_t nc_put_u32(char *out, uint32_t v)
{
    char digits[10];
    uint32_t n = 0, i;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    for (i = 0; i < n; i++) {
        out[i] = digits[n - 1 - i];
    }
    return n;
}

/**
 * Read the record prefix `<level>[seconds.microseconds] ` that begins the
 * `n` characters of `nc_line` into `t->level` and `t->usec`, the stamp as one
 * number of microseconds with no leading zeros, and return where the text
 * after it starts; 0, leaving both as they were, when the line has none.
 */
static uint32_t nc_prefix(struct nc_target *t, uint32_t n)
{
    char usec[sizeof(t->usec)];
    uint32_t k = 1, u = 0, j;
    int level = 0;

    if (n == 0 || nc_line[0] != '<') {
        return 0;
    }
    while (k < 4 && k < n && nc_line[k] >= '0' && nc_line[k] <= '9') {
        level = level * 10 + (nc_line[k++] - '0');
    }
    if (k == 1 || k + 1 >= n || nc_line[k] != '>' || nc_line[k + 1] != '[') {
        return 0;
    }
    for (k += 2; k < n && u + 1 < sizeof(usec); k++) {
        char c = nc_line[k];

        if (c != '.' && (c < '0' || c > '9')) {
            break;
        }
        if (c != '.' && (u > 0 || c != '0')) {
            usec[u++] = c;
        }
    }
    if (k + 1 >= n || nc_line[k] != ']' || nc_line[k + 1] != ' ') {
        return 0;
    }
    if (u == 0) {
        usec[u++] = '0';
    }
    usec[u] = '\0';
    for (j = 0; j <= u; j++) {
        t->usec[j] = usec[j];
    }
    t->level = level;
    return k + 2;
}

/**
 * Write the `n` characters of `nc_line` in Linux's extended form to `nc_ext`
 * and return its length: the release and a comma for `r`, then
 * `level,seq,usec,flags;` as `info_print_ext_header` writes it, then the
 * text with every byte below a space, from 127 up, and the backslash written
 * `\xNN`, and a newline, as `msg_add_ext_text` does (kernel/printk/printk.c).
 *
 * The level and the stamp come from the record's own prefix, which the text
 * then leaves out (`nc_prefix`); the flags are `-`. A piece of a longer line
 * after its first has no prefix of its own, so it takes the last record's
 * level and stamp and the flag `c`, which Linux gives a continuation.
 */
static uint32_t nc_extend(struct nc_target *t, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t p = 0, i, j;
    char flag;

    if (n > 0 && nc_line[n - 1] == '\n') {
        n--;
    }
    i = nc_prefix(t, n);
    flag = i == 0 ? 'c' : '-';
    if (t->usec[0] == '\0') {
        t->usec[0] = '0';
    }
    if (t->release) {
        for (j = 0; UTS_RELEASE[j] != '\0'; j++) {
            nc_ext[p++] = UTS_RELEASE[j];
        }
        nc_ext[p++] = ',';
    }
    p += nc_put_u32(nc_ext + p, (uint32_t)t->level);
    nc_ext[p++] = ',';
    p += nc_put_u32(nc_ext + p, t->seq);
    nc_ext[p++] = ',';
    for (j = 0; t->usec[j] != '\0'; j++) {
        nc_ext[p++] = t->usec[j];
    }
    nc_ext[p++] = ',';
    nc_ext[p++] = flag;
    nc_ext[p++] = ';';
    for (; i < n; i++) {
        uint8_t c = (uint8_t)nc_line[i];

        if (c < ' ' || c >= 127 || c == '\\') {
            nc_ext[p++] = '\\';
            nc_ext[p++] = 'x';
            nc_ext[p++] = hex[c >> 4];
            nc_ext[p++] = hex[c & 0xF];
        } else {
            nc_ext[p++] = (char)c;
        }
    }
    nc_ext[p++] = '\n';
    return p;
}

/** Where the payload starts in `nc_pkt` for `t`. */
static uint32_t nc_payload_at(const struct nc_target *t)
{
    return (t->ipv6 ? NC_IP6_HDR : NC_IP4_HDR) + NC_UDP_HDR;
}

/**
 * Send the `plen` bytes at `nc_pkt + nc_payload_at(t)` to `t` from `src` as
 * one UDP datagram, built as `netpoll_send_udp` builds it. Returns what
 * `ethernet_tx` returns. Under `net_lock`.
 */
static int nc_udp_tx(struct nc_target *t, const uint8_t *src, uint32_t plen)
{
    uint32_t ip = t->ipv6 ? NC_IP6_HDR : NC_IP4_HDR;
    uint8_t *udp = nc_pkt + ip;
    uint32_t ulen = NC_UDP_HDR + plen;
    uint8_t pseudo[4];
    uint32_t sum;
    uint16_t check;
    int i;

    write16be(udp, t->local_port);
    write16be(udp + 2, t->remote_port);
    write16be(udp + 4, (uint16_t)ulen);
    write16be(udp + 6, 0);
    if (t->ipv6) {
        sum = nc_sum(0, src, 16);
        sum = nc_sum(sum, t->remote_ip, 16);
        write32be(pseudo, ulen);
        sum = nc_sum(sum, pseudo, 4);
        write32be(pseudo, IP_PROTO_UDP);
        sum = nc_sum(sum, pseudo, 4);
    } else {
        sum = nc_sum(0, src, 4);
        sum = nc_sum(sum, t->remote_ip, 4);
        pseudo[0] = 0;
        pseudo[1] = IP_PROTO_UDP;
        write16be(pseudo + 2, (uint16_t)ulen);
        sum = nc_sum(sum, pseudo, 4);
    }
    check = (uint16_t)(~nc_sum(sum, udp, ulen) & 0xFFFFu);
    write16be(udp + 6, check == 0 ? 0xFFFFu : check);

    if (t->ipv6) {
        nc_pkt[0] = 0x60;
        nc_pkt[1] = 0;
        nc_pkt[2] = 0;
        nc_pkt[3] = 0;
        write16be(nc_pkt + 4, (uint16_t)ulen);
        nc_pkt[6] = IP_PROTO_UDP;
        nc_pkt[7] = 32;
        for (i = 0; i < 16; i++) {
            nc_pkt[8 + i] = src[i];
            nc_pkt[24 + i] = t->remote_ip[i];
        }
        return ethernet_tx(t->nd, t->remote_mac, ETH_TYPE_IPV6, nc_pkt, (uint16_t)(ip + ulen));
    }
    nc_pkt[0] = 0x45;
    nc_pkt[1] = 0;
    write16be(nc_pkt + 2, (uint16_t)(ip + ulen));
    write16be(nc_pkt + 4, ++nc_ip_id);
    write16be(nc_pkt + 6, 0);
    nc_pkt[8] = 64;
    nc_pkt[9] = IP_PROTO_UDP;
    write16be(nc_pkt + 10, 0);
    for (i = 0; i < 4; i++) {
        nc_pkt[12 + i] = src[i];
        nc_pkt[16 + i] = t->remote_ip[i];
    }
    write16be(nc_pkt + 10, ip_checksum(nc_pkt, NC_IP4_HDR));
    return ethernet_tx(t->nd, t->remote_mac, ETH_TYPE_IP, nc_pkt, (uint16_t)(ip + ulen));
}

/**
 * Send the extended message of `len` bytes in `nc_ext` to `t`: whole when it
 * fits NETCONSOLE_CHUNK_MAX, and otherwise in pieces, each the header with
 * `,ncfrag=OFFSET/TOTAL` before its `;` and as much of the text after it as
 * fits, as Linux's `send_ext_msg_udp` sends one. Returns what the last
 * `nc_udp_tx` returned, stopping at the first refusal.
 */
static int nc_send_ext(struct nc_target *t, const uint8_t *src, uint32_t len)
{
    uint8_t *out = nc_pkt + nc_payload_at(t);
    uint32_t header, body, offset = 0, i;
    int r = NETDEV_TX_OK;

    if (len <= NETCONSOLE_CHUNK_MAX) {
        for (i = 0; i < len; i++) {
            out[i] = (uint8_t)nc_ext[i];
        }
        return nc_udp_tx(t, src, len);
    }
    for (header = 0; nc_ext[header] != ';'; header++) {
        /* the header is everything before the first semicolon */
    }
    body = len - header - 1;
    while (offset < body && r == NETDEV_TX_OK) {
        uint32_t p = header, chunk;

        for (i = 0; i < header; i++) {
            out[i] = (uint8_t)nc_ext[i];
        }
        out[p++] = ',';
        for (i = 0; "ncfrag="[i] != '\0'; i++) {
            out[p++] = (uint8_t)"ncfrag="[i];
        }
        p += nc_put_u32((char *)out + p, offset);
        out[p++] = '/';
        p += nc_put_u32((char *)out + p, body);
        out[p++] = ';';
        chunk = body - offset;
        if (chunk > NETCONSOLE_CHUNK_MAX - p) {
            chunk = NETCONSOLE_CHUNK_MAX - p;
        }
        for (i = 0; i < chunk; i++) {
            out[p + i] = (uint8_t)nc_ext[header + 1 + offset + i];
        }
        r = nc_udp_tx(t, src, p + chunk);
        offset += chunk;
    }
    return r;
}

/**
 * Send `t` its next line before the place `end`. Returns 1 when one was sent,
 * or dropped as too long for the interface, which no later attempt would
 * change; 0 when there is none whole yet, or the drain must wait, with
 * `t->held` set. Under `net_lock`.
 */
static int nc_send_line(struct nc_target *t, uint32_t end)
{
    uint8_t src[16];
    uint32_t n, i, from;
    int r;

    if (!netdev_running(t->nd) || !nc_source(t, src)) {
        t->held = 1;
        return 0;
    }
    n = nc_take(t->tail, end, t->extended ? NC_EXT_LINE : NETCONSOLE_CHUNK_MAX);
    if (n == 0) {
        return 0;
    }
    if (t->extended) {
        r = nc_send_ext(t, src, nc_extend(t, n));
    } else {
        for (i = 0; i < n; i++) {
            nc_pkt[nc_payload_at(t) + i] = (uint8_t)nc_line[i];
        }
        r = nc_udp_tx(t, src, n);
    }
    if (r == NETDEV_TX_DROPPED) {
        t->held = 1;
        return 0;
    }
    from = t->tail + n;
    t->tail = from >= NETCONSOLE_RING ? from - NETCONSOLE_RING : from;
    t->held = 0;
    t->seq++;
    if (r == NETDEV_TX_OK) {
        t->datagrams++;
    }
    return 1;
}

/** Send each target the whole lines the ring holds, as `netconsole_flush`
 * says; without the frames, `nc_tail` then follows the target furthest
 * behind. */
static void nc_flush_targets(void)
{
    uint32_t flags = net_lock();
    uint32_t end;
    struct nc_target *t;

    if (!nc_targets || nc_sending) {
        net_unlock(flags);
        return;
    }
    nc_sending = 1;
    end = nc_head;
    net_unlock(flags);

    for (t = nc_targets; t; t = t->next) {
        int sent;

        do {
            flags = net_lock();
            sent = nc_send_line(t, end);
            net_unlock(flags);
        } while (sent);
    }

    flags = net_lock();
    if (!nc_enabled) {
        uint32_t most = 0;

        for (t = nc_targets; t; t = t->next) {
            if (nc_behind(t->tail) >= most) {
                most = nc_behind(t->tail);
                nc_tail = t->tail;
            }
        }
    }
    nc_sending = 0;
    net_unlock(flags);
}

void netconsole_flush(void)
{
    nc_flush_frames();
    nc_flush_targets();
}

/* ---- reading netconsole= ---- */

/** The first `c` in `s`, or NULL. */
static char *nc_strchr(char *s, char c)
{
    for (; *s != '\0'; s++) {
        if (*s == c) {
            return s;
        }
    }
    return 0;
}

/** Whether `s` has a `c`. */
static int nc_has(const char *s, char c)
{
    for (; *s != '\0'; s++) {
        if (*s == c) {
            return 1;
        }
    }
    return 0;
}

/** Read `s` whole as a decimal port, as `kstrtou16` does; 0 when it is not
 * one. */
static int nc_parse_port(const char *s, uint16_t *out)
{
    uint32_t v = 0;

    if (*s == '+') {
        s++;
    }
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > 0xFFFFu) {
            return 0;
        }
    }
    *out = (uint16_t)v;
    return 1;
}

/**
 * Read `s` whole as an address, as Linux's `netpoll_parse_ip_addr` does: a
 * dotted IPv4 address when it has no colon, else IPv6. Returns 0 for IPv4,
 * 1 for IPv6, -1 for neither; `out` gets the address in network order.
 */
static int nc_parse_ip(const char *s, uint8_t *out)
{
    const char *p = s;
    uint32_t len = 0;
    int part;

    while (s[len] != '\0') {
        len++;
    }
    if (!nc_has(s, ':')) {
        for (part = 0; part < 4; part++) {
            uint32_t v = 0;
            int digits = 0;

            while (*p >= '0' && *p <= '9' && digits < 3) {
                v = v * 10u + (uint32_t)(*p++ - '0');
                digits++;
            }
            if (digits == 0 || v > 255 || *p != (part == 3 ? '\0' : '.')) {
                return -1;
            }
            out[part] = (uint8_t)v;
            p++;
        }
        return 0;
    }
    return ip6_parse(s, len, out) == 0 ? 1 : -1;
}

/** Read `s` as a station address, six pairs of hexadecimal digits separated
 * by colons, as Linux's `mac_pton` does; 0 when it is not one. */
static int nc_parse_mac(const char *s, uint8_t *out)
{
    int i, k;

    for (i = 0; i < 6; i++) {
        uint8_t v = 0;

        for (k = 0; k < 2; k++) {
            char c = s[i * 3 + k];

            if (c >= '0' && c <= '9') {
                v = (uint8_t)(v * 16 + (c - '0'));
            } else if (c >= 'a' && c <= 'f') {
                v = (uint8_t)(v * 16 + (c - 'a' + 10));
            } else if (c >= 'A' && c <= 'F') {
                v = (uint8_t)(v * 16 + (c - 'A' + 10));
            } else {
                return 0;
            }
        }
        if (i != 5 && s[i * 3 + 2] != ':') {
            return 0;
        }
        out[i] = v;
    }
    return 1;
}

/** The interface whose station address is `mac`, or NULL. */
static struct netdev *nc_netdev_by_mac(const uint8_t *mac)
{
    struct netdev *nd;
    int n, i;

    for (n = 0; (nd = netdev_at(n)) != 0; n++) {
        for (i = 0; i < 6 && nd->mac[i] == mac[i]; i++) {
            /* compare each byte */
        }
        if (i == 6) {
            return nd;
        }
    }
    return 0;
}

/** Where the interface of a target is named: by name, or by station address
 * when `by_mac` is set. */
struct nc_dev {
    const char *name;
    uint8_t mac[6];
    int by_mac;
};

/**
 * Read `cur`, a target past its `+` and `r`, into `t` and `dev` as Linux's
 * `netpoll_parse_options` reads one (net/core/netpoll.c), writing NULs over
 * its separators. Returns NULL, or where the text stopped making sense.
 */
static char *nc_parse_options(char *cur, struct nc_target *t, struct nc_dev *dev)
{
    char *delim;
    int ipversion_set = 0, v6;

    if (*cur != '@') {
        if ((delim = nc_strchr(cur, '@')) == 0) {
            return cur;
        }
        *delim = '\0';
        if (!nc_parse_port(cur, &t->local_port)) {
            return cur;
        }
        cur = delim;
    }
    cur++;
    if (*cur != '/') {
        ipversion_set = 1;
        if ((delim = nc_strchr(cur, '/')) == 0) {
            return cur;
        }
        *delim = '\0';
        if ((v6 = nc_parse_ip(cur, t->local_ip)) < 0) {
            return cur;
        }
        t->ipv6 = v6;
        cur = delim;
    }
    cur++;
    if (*cur != ',') {
        if ((delim = nc_strchr(cur, ',')) == 0) {
            return cur;
        }
        *delim = '\0';
        if (!nc_has(cur, ':')) {
            dev->name = cur;
        } else if (nc_parse_mac(cur, dev->mac)) {
            dev->by_mac = 1;
        } else {
            return cur;
        }
        cur = delim;
    }
    cur++;
    if (*cur != '@') {
        if ((delim = nc_strchr(cur, '@')) == 0) {
            return cur;
        }
        *delim = '\0';
        if (!nc_parse_port(cur, &t->remote_port)) {
            return cur;
        }
        cur = delim;
    }
    cur++;
    if ((delim = nc_strchr(cur, '/')) == 0) {
        return cur;
    }
    *delim = '\0';
    if ((v6 = nc_parse_ip(cur, t->remote_ip)) < 0 || (ipversion_set && t->ipv6 != v6)) {
        return cur;
    }
    t->ipv6 = v6;
    cur = delim + 1;
    if (*cur != '\0' && !nc_parse_mac(cur, t->remote_mac)) {
        return cur;
    }
    return 0;
}

/**
 * Read one target, `cur`, into `t` as Linux's `alloc_and_init` and
 * `alloc_param_target` read one (drivers/net/netconsole.c): Linux's ports,
 * `eth0` and the broadcast station address unless it names others, `+` and
 * `r` first. Then find its interface, which may be any card but not `lo`.
 * Returns 1, or 0 having logged why not.
 */
static int nc_parse_target(char *cur, struct nc_target *t)
{
    struct nc_dev dev;
    char *bad;
    int i;

    dev.name = NETDEV_IFNAME;
    dev.by_mac = 0;
    t->local_port = NETCONSOLE_LOCAL_PORT;
    t->remote_port = NETCONSOLE_REMOTE_PORT;
    for (i = 0; i < 6; i++) {
        t->remote_mac[i] = 0xFF;
    }
    if (*cur == '+') {
        t->extended = 1;
        cur++;
    }
    if (*cur == 'r') {
        if (!t->extended) {
            klog(KLOG_ERR, "netconsole", "refused: %s: r needs the extended form, +", cur);
            return 0;
        }
        t->release = 1;
        cur++;
    }
    if ((bad = nc_parse_options(cur, t, &dev)) != 0) {
        klog(KLOG_ERR, "netconsole", "refused: cannot read the target at '%s'", bad);
        return 0;
    }
    t->nd = dev.by_mac ? nc_netdev_by_mac(dev.mac) : netdev_by_name(dev.name);
    if (!t->nd || t->nd == netdev_loopback()) {
        klog(KLOG_ERR, "netconsole", "refused: %s: no such card",
             dev.by_mac ? "station address" : dev.name);
        return 0;
    }
    return 1;
}

/** Log the target `t` as Linux's `netpoll_print_options` gives one. */
static void nc_print_target(const struct nc_target *t)
{
    char local[40], remote[40];

    if (t->ipv6) {
        ip6_format(t->local_ip, local);
        ip6_format(t->remote_ip, remote);
    } else {
        local[0] = remote[0] = '\0';
    }
    if (t->ipv6) {
        klog(KLOG_INFO, "netconsole", "udp: %d@%s/%s,%d@%s/%X%X:%X%X:%X%X:%X%X:%X%X:%X%X%s%s",
             (int)t->local_port, nc_zero(t->local_ip, 16) ? "" : local, t->nd->name,
             (int)t->remote_port, remote, (unsigned)(t->remote_mac[0] >> 4),
             (unsigned)(t->remote_mac[0] & 0xF), (unsigned)(t->remote_mac[1] >> 4),
             (unsigned)(t->remote_mac[1] & 0xF), (unsigned)(t->remote_mac[2] >> 4),
             (unsigned)(t->remote_mac[2] & 0xF), (unsigned)(t->remote_mac[3] >> 4),
             (unsigned)(t->remote_mac[3] & 0xF), (unsigned)(t->remote_mac[4] >> 4),
             (unsigned)(t->remote_mac[4] & 0xF), (unsigned)(t->remote_mac[5] >> 4),
             (unsigned)(t->remote_mac[5] & 0xF), t->extended ? ", extended" : "",
             t->release ? " with the release" : "");
        return;
    }
    klog(KLOG_INFO, "netconsole",
         "udp: %d@%d.%d.%d.%d/%s,%d@%d.%d.%d.%d/%X%X:%X%X:%X%X:%X%X:%X%X:%X%X%s%s",
         (int)t->local_port, (int)t->local_ip[0], (int)t->local_ip[1], (int)t->local_ip[2],
         (int)t->local_ip[3], t->nd->name, (int)t->remote_port, (int)t->remote_ip[0],
         (int)t->remote_ip[1], (int)t->remote_ip[2], (int)t->remote_ip[3],
         (unsigned)(t->remote_mac[0] >> 4), (unsigned)(t->remote_mac[0] & 0xF),
         (unsigned)(t->remote_mac[1] >> 4), (unsigned)(t->remote_mac[1] & 0xF),
         (unsigned)(t->remote_mac[2] >> 4), (unsigned)(t->remote_mac[2] & 0xF),
         (unsigned)(t->remote_mac[3] >> 4), (unsigned)(t->remote_mac[3] & 0xF),
         (unsigned)(t->remote_mac[4] >> 4), (unsigned)(t->remote_mac[4] & 0xF),
         (unsigned)(t->remote_mac[5] >> 4), (unsigned)(t->remote_mac[5] & 0xF),
         t->extended ? ", extended" : "", t->release ? " with the release" : "");
}

/** Choose the boot id the frames carry. It tells boots apart in a capture
 * and need not be unpredictable; it is never 0. */
static void nc_choose_boot_id(void)
{
    nc_boot_id = krandom_u32();
    if (nc_boot_id == 0) {
        nc_boot_id = 1;
    }
}

/**
 * The targets are read in order and kept in that order. They and the frames
 * start together, each at the oldest character the ring holds, so each is
 * sent the log from the boot's first record, as Linux's CON_PRINTBUFFER
 * replays the log buffer to a new console (`init_netconsole`). Started one at
 * a time, the first one's drain, which its own log line begins, would free
 * what the next had not been sent.
 */
int netconsole_setup(const char *config, int frames)
{
    static char copy[512];
    char *input = copy, *target;
    struct nc_target *first = 0, **link = &first, *t;
    uint32_t i, flags;
    int count = 0;

    if (nc_setup_done) {
        return 0;
    }
    nc_setup_done = 1;
    for (i = 0; i + 1 < sizeof(copy) && config[i] != '\0'; i++) {
        copy[i] = config[i];
    }
    copy[i] = '\0';

    while (input) {
        char *semi = nc_strchr(input, ';');

        target = input;
        if (semi) {
            *semi = '\0';
            input = semi + 1;
        } else {
            input = 0;
        }
        t = (struct nc_target *)kzalloc(sizeof(*t));
        if (!t) {
            klog(KLOG_ERR, "netconsole", "refused: no memory for a target");
            break;
        }
        if (!nc_parse_target(target, t)) {
            kfree(t);
            continue;
        }
        nc_print_target(t);
        *link = t;
        link = &t->next;
        count++;
    }
    if (count > 0) {
        klog(KLOG_INFO, "netconsole", "started: network logging, targets %d", count);
    }
    if (frames && !nc_enabled) {
        nc_choose_boot_id();
    }

    flags = net_lock();
    for (t = first; t; t = t->next) {
        t->tail = nc_tail;
    }
    nc_targets = first;
    if (frames) {
        nc_enabled = 1;
    }
    net_unlock(flags);
    netconsole_flush();
    return count;
}

void netconsole_enable(void)
{
    if (nc_enabled) {
        return;
    }
    nc_choose_boot_id();
    nc_enabled = 1;
    netconsole_flush();
}

int netconsole_is_enabled(void)
{
    return nc_enabled || nc_targets != 0;
}

void netconsole_print_info(void)
{
    struct nc_target *t;

    if (nc_enabled) {
        klog(KLOG_INFO, "netconsole",
             "broadcast, ethertype 0x%x, boot id 0x%x, "
             "%d frames, %d characters dropped",
             (int)NETCONSOLE_ETHERTYPE, (int)nc_boot_id, (int)nc_frames, (int)nc_dropped);
    }
    for (t = nc_targets; t; t = t->next) {
        klog(KLOG_INFO, "netconsole", "udp: port %d of %s: %d datagrams%s, %d characters dropped",
             (int)t->remote_port, t->nd->name, (int)t->datagrams, t->held ? ", held" : "",
             (int)nc_dropped);
    }
}
