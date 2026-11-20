/**
 * vhostmusl: check /dev/vhost-net as QEMU's `vhost=on` drives it, with the
 * program in the guest's place.
 *
 * The program makes the tap `vh0` at 10.9.0.1/24 with packet information,
 * gives /dev/vhost-net a buffer of its own as the guest's memory, at a
 * guest-physical address of
 * its choosing, lays a receive and a transmit ring out in it as a virtio
 * driver does, with an eventfd for each ring's kick and call, and gives
 * both rings the tap, with no receive buffer offered yet. A datagram sent
 * to 10.9.0.2 makes the stack ask for that address, and the ARP request
 * waits in the tap; once the program offers a buffer and kicks, the kernel
 * thread writes it into the buffer after a virtio header with num_buffers 1 and
 * signals the receive ring's call. The program answers it on the transmit
 * ring and kicks. The answer fills a 1514-byte frame, and a packet
 * socket checks that the thread gives the whole answer to the stack. The
 * datagram that waited for it comes into the receive ring, and the transmit
 * ring's call is signalled. A second datagram, sent once a buffer is offered,
 * comes into it: in the sender's context, before its send returns, when
 * the device already holds the buffer's page and no frame waiting in the
 * tap took the buffer first, and otherwise from the thread, either way
 * with its call.
 *
 * Other frames the stack sends on the tap, IPv6's among them, may take
 * receive buffers too, so the program looks for its frames among those the
 * ring used. It offers one receive buffer at a time, so that the ring holds
 * at most one frame the program has not counted. A wait is a read of a call
 * eventfd, which the kernel signals once for each frame, after its used
 * index, under the lock that VHOST_SET_VRING_CALL takes. After each frame
 * the program sets the receive call again, so that every call signalled for
 * that frame is in the eventfd, reads what it holds without waiting, and
 * requires the calls read to equal the used entries: a missing call or an
 * extra one fails the frame it belongs to, and none stands in for another's.
 * The program counts the used entries from the start, past the 16-bit used
 * index's wrap, so the count holds however many frames come.
 * Once the rings let the tap go, the calls must still equal the used
 * entries. Every wait blocks on an eventfd, with no clock, so a frame or a
 * call that never comes leaves the program waiting.
 *
 * Prints `VHOSTMUSL OK` and exits 0 on success, and prints
 * `VHOSTMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists; a failure leaves the checks after it not run.
 * Needs root, for the tap and the device.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "../probe_report.h"

extern char **environ;

#define TUNSETIFF             0x400454caU
#define VHOST_GET_FEATURES    0x8008af00U
#define VHOST_SET_FEATURES    0x4008af00U
#define VHOST_SET_OWNER       0x0000af01U
#define VHOST_SET_MEM_TABLE   0x4008af03U
#define VHOST_SET_VRING_NUM   0x4008af10U
#define VHOST_SET_VRING_ADDR  0x4028af11U
#define VHOST_SET_VRING_BASE  0x4008af12U
#define VHOST_SET_VRING_KICK  0x4008af20U
#define VHOST_SET_VRING_CALL  0x4008af21U
#define VHOST_NET_SET_BACKEND 0x4008af30U

#define F_NET_HDR   (1ULL << 27)
#define F_VERSION_1 (1ULL << 32)
#define HDR         12 /* the 1.0 header, with num_buffers */
#define PI_LEN      4  /* struct tun_pi */
#define ETH_HDR     14
#define ETH_ARP     0x0806
#define MTU         1500

#define GPA      0x40000000ULL /* the guest-physical address of the memory */
#define MEMSIZE  (64 * 1024)
#define NUM      16 /* entries of each ring */
#define RX_DESC  0x0000
#define RX_AVAIL 0x0400
#define RX_USED  0x0800
#define TX_DESC  0x1000
#define TX_AVAIL 0x1400
#define TX_USED  0x1800
#define RX_BUF   0x2000 /* NUM receive buffers of BUFLEN */
#define TX_BUF   0xc000
#define BUFLEN   2048

enum {
    TAP_OPEN,
    TAP_IFF,
    TAP_ADDRESS,
    MEMORY,
    VHOST_OPEN,
    OWNER,
    FEATURES,
    SET_FEATURES,
    MEM_TABLE,
    EVENTFDS,
    RINGS,
    RING_FILES,
    BACKEND,
    SEND,
    HELD,
    RX_KICK,
    ARP_FRAME,
    ARP_HEADER,
    TX_KICK,
    TX_CALL,
    TX_USED_ENTRY,
    TX_FULL_FRAME,
    DATAGRAM,
    DIRECT_SEND,
    DIRECT_FRAME,
    DIRECT_CALL,
    DETACH,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"tap-open", "could not open /dev/net/tun"},
    {"tap-iff", "TUNSETIFF did not make vh0"},
    {"tap-address", "could not give vh0 10.9.0.1/24 and set it up"},
    {"memory", "could not map the guest's memory"},
    {"vhost-open", "could not open /dev/vhost-net"},
    {"owner", "VHOST_SET_OWNER failed"},
    {"features", "VHOST_GET_FEATURES lacks the header or the 1.0 layout"},
    {"set-features", "VHOST_SET_FEATURES of the header and the 1.0 layout failed"},
    {"mem-table", "VHOST_SET_MEM_TABLE failed"},
    {"eventfds", "could not make the four eventfds"},
    {"rings", "a ring's size, base or address was refused"},
    {"ring-files", "a ring's kick or call was refused"},
    {"backend", "VHOST_NET_SET_BACKEND of the tap failed"},
    {"send", "could not send the first datagram to 10.9.0.2"},
    {"held", "a frame was written with no buffer offered"},
    {"rx-kick", "could not kick the receive ring"},
    {"arp-frame",
     "the ARP request for 10.9.0.2 did not come, with one call, once a buffer was offered"},
    {"arp-header", "the receive header is not of no offload with num_buffers 1"},
    {"tx-kick", "could not kick the transmit ring"},
    {"tx-call", "the transmit ring's call did not come"},
    {"tx-used", "the transmit ring did not use the answer's chain"},
    {"tx-full-frame", "the TAP did not deliver the whole Ethernet frame"},
    {"datagram", "the first datagram did not come into the receive ring after the answer, or not "
                 "with one call"},
    {"direct-send", "could not offer a buffer and then send the second datagram"},
    {"direct-frame", "the second datagram did not come into the receive ring"},
    {"direct-call", "the receive ring's call was not signalled once for the second datagram"},
    {"detach",
     "VHOST_NET_SET_BACKEND of -1 failed, or the receive calls then do not equal the used entries"},
};

static const unsigned char peer[6] = {2, 0, 0, 0, 0, 2};
static unsigned char *mem; /* the guest's memory */
/* Receive buffers offered, and the receive ring's used entries looked at,
 * since the start: the ring's 16-bit indices are these, modulo 65536. */
static uint64_t rx_avail_idx, rx_seen;
static uint64_t rx_calls; /* the receive ring's calls read, summed */
/* The descriptors and the eventfds: rx kick, rx call, tx kick, tx call. */
static int tap = -1, vh = -1, sock = -1, arp_sock = -1, ev[4] = {-1, -1, -1, -1};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with its text, those after it as not run, and
 * the report's end; with `check` CHECKS, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, checks[i].text);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Report the failed check `check` and return 1, the probe's failure
 * status. */
static int fail(int check)
{
    printf("VHOSTMUSL FAIL (%s: %s)\n", checks[check].text, strerror(errno));
    report_at(check);
    return 1;
}

/** The 16-bit word at offset `off` of the guest's memory. */
static uint16_t get16(int off)
{
    uint16_t v;

    memcpy(&v, mem + off, 2);
    return v;
}

/** Store the 16-bit word `v` at offset `off`. */
static void put16(int off, uint16_t v)
{
    memcpy(mem + off, &v, 2);
}

/** Write descriptor `i` of the table at `table`: `len` bytes at the guest
 * address of offset `off`, with `flags`. */
static void desc(int table, int i, int off, uint32_t len, uint16_t flags)
{
    uint64_t addr = GPA + (uint64_t)off;
    uint16_t next = 0;

    memcpy(mem + table + 16 * i, &addr, 8);
    memcpy(mem + table + 16 * i + 8, &len, 4);
    memcpy(mem + table + 16 * i + 12, &flags, 2);
    memcpy(mem + table + 16 * i + 14, &next, 2);
}

/** Offer receive buffer `i`, of BUFLEN bytes, on the receive ring. */
static void offer_rx(int i)
{
    desc(RX_DESC, i, RX_BUF + BUFLEN * i, BUFLEN, 2 /* VRING_DESC_F_WRITE */);
    put16(RX_AVAIL + 4 + 2 * (int)(rx_avail_idx % NUM), (uint16_t)i);
    __sync_synchronize();
    rx_avail_idx++;
    put16(RX_AVAIL + 2, (uint16_t)rx_avail_idx);
}

/** A ring request's argument, a ring's index and a number. */
static int ring2(int fd, unsigned long req, unsigned int index, int num)
{
    unsigned int st[2];

    st[0] = index;
    st[1] = (unsigned int)num;
    return ioctl(fd, req, st);
}

/** VHOST_SET_VRING_ADDR of ring `index` at the offsets given. */
static int ring_addr(int fd, unsigned int index, int d, int u, int a)
{
    uint64_t addr[5];

    memset(addr, 0, sizeof(addr));
    memcpy(&addr[0], &index, 4);
    addr[1] = (uintptr_t)mem + d;
    addr[2] = (uintptr_t)mem + u;
    addr[3] = (uintptr_t)mem + a;
    return ioctl(fd, VHOST_SET_VRING_ADDR, addr);
}

/** Give vh0 the address 10.9.0.1/24 and set it up: 0, or -1. */
static int address_tap(void)
{
    struct ifreq ifr;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    int s = socket(AF_INET, SOCK_DGRAM, 0), r = -1;

    if (s < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "vh0");
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = inet_addr("10.9.0.1");
    if (ioctl(s, SIOCSIFADDR, &ifr) == 0) {
        sin->sin_addr.s_addr = inet_addr("255.255.255.0");
        if (ioctl(s, SIOCSIFNETMASK, &ifr) == 0 && ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
            ifr.ifr_flags |= IFF_UP;
            r = ioctl(s, SIOCSIFFLAGS, &ifr);
        }
    }
    close(s);
    return r;
}

/** Send the datagram `text` to 10.9.0.2 port 9 on `s`: 0, or -1. */
static int send_to_peer(int s, const char *text)
{
    struct sockaddr_in to;

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = inet_addr("10.9.0.2");
    return sendto(s, text, strlen(text), 0, (struct sockaddr *)&to, sizeof(to)) ==
                   (ssize_t)strlen(text)
               ? 0
               : -1;
}

/** Whether the frame `f` of `n` bytes is an ARP request for 10.9.0.2. */
static int is_arp_request(const unsigned char *f, uint32_t n)
{
    return n >= 42 && f[12] == 8 && f[13] == 6 && f[21] == 1 && f[38] == 10 && f[39] == 9 &&
           f[40] == 0 && f[41] == 2;
}

/** Whether the frame `f` of `n` bytes is the UDP datagram `text` to the
 * peer's station address. */
static int is_datagram(const unsigned char *f, uint32_t n, const char *text)
{
    size_t len = strlen(text);

    return n >= 42 + len && f[12] == 8 && f[13] == 0 && f[23] == 17 && memcmp(f, peer, 6) == 0 &&
           memcmp(f + 42, text, len) == 0;
}

/**
 * Read the receive ring's used index into `*n` as a count of used entries
 * since the start: the entries seen, and the index's distance past them,
 * modulo 65536. 0; -1 with errno EPROTO when that distance passes the
 * buffers offered and not yet seen, which a device cannot use.
 */
static int rx_used(uint64_t *n)
{
    uint16_t used, d;

    __sync_synchronize();
    used = get16(RX_USED + 2);
    d = (uint16_t)(used - (uint16_t)rx_seen);
    if (d > rx_avail_idx - rx_seen) {
        fprintf(stderr, "vhostmusl: receive used index %u past the %llu buffers offered\n",
                (unsigned)used, (unsigned long long)rx_avail_idx);
        errno = EPROTO;
        return -1;
    }
    *n = rx_seen + d;
    return 0;
}

/**
 * Look at the receive ring's used entries not yet seen for the frame
 * `match` accepts with `text`: 1 with the buffer's offset in `*at`, 0 once
 * every used entry is seen, or -1 as `rx_used` fails, for a used index no
 * device can give.
 */
static int find_rx(int (*match)(const unsigned char *, uint32_t, const char *), const char *text,
                   int *at)
{
    uint64_t end;

    if (rx_used(&end) != 0) {
        return -1;
    }
    while (rx_seen != end) {
        uint32_t id, len;
        int e = RX_USED + 4 + 8 * (int)(rx_seen % NUM);

        memcpy(&id, mem + e, 4);
        memcpy(&len, mem + e + 4, 4);
        rx_seen++;
        if (id < NUM && len >= HDR + PI_LEN && len <= BUFLEN &&
            match(mem + RX_BUF + BUFLEN * id + HDR + PI_LEN, len - HDR - PI_LEN, text)) {
            *at = RX_BUF + BUFLEN * (int)id;
            return 1;
        }
    }
    return 0;
}

/** `is_arp_request` in `find_rx`'s form. */
static int arp_match(const unsigned char *f, uint32_t n, const char *text)
{
    (void)text;
    return is_arp_request(f, n);
}

/** Print the receive calls read and the used entries seen with them, after
 * a count that a call signalled once for each frame cannot give. */
static void miscount(uint64_t used)
{
    fprintf(stderr, "vhostmusl: receive calls: %llu for %llu used entries\n",
            (unsigned long long)rx_calls, (unsigned long long)used);
    errno = EPROTO;
}

/**
 * Read the receive ring's call `call`, adding the signals it counted to
 * `rx_calls`: 0; -1 when the read fails, as `rx_used` fails, or when the
 * calls read outnumber the used entries read after them, which a call
 * signalled once for each frame after its used index cannot do.
 */
static int take_call(int call)
{
    uint64_t v, used;

    if (read(call, &v, 8) != 8) {
        return -1;
    }
    rx_calls += v;
    if (rx_used(&used) != 0) {
        return -1;
    }
    if (rx_calls > used) {
        miscount(used);
        return -1;
    }
    return 0;
}

/** Count what the receive ring's call `call` holds now, without waiting:
 * 0, or -1 when poll fails or reports no data, or as `take_call` fails. */
static int drain_calls(int call)
{
    struct pollfd p;
    int r;

    p.fd = call;
    p.events = POLLIN;
    while ((r = poll(&p, 1, 0)) > 0) {
        if (!(p.revents & POLLIN)) {
            errno = EIO;
            return -1;
        }
        if (take_call(call) != 0) {
            return -1;
        }
    }
    return r;
}

/**
 * Settle the count of the receive ring's calls `call` once the ring holds
 * no buffer: set the call again, which waits for the device's lock and so
 * for every call already signalled, read what the eventfd holds, and
 * require the calls read to equal the used entries. 0, or -1 for a failed
 * request or read, a used index no device can give, or an unequal count.
 */
static int settle_calls(int call)
{
    uint64_t used;

    if (ring2(vh, VHOST_SET_VRING_CALL, 0, call) != 0 || drain_calls(call) != 0 ||
        rx_used(&used) != 0) {
        return -1;
    }
    if (rx_calls != used) {
        miscount(used);
        return -1;
    }
    return 0;
}

/** Offer the next receive buffer, unless one offered is not yet used and
 * looked at, and kick the receive ring, for a frame waiting in the tap: 0,
 * or -1 when the kick fails. So the ring never holds more than one buffer,
 * and no frame comes but the one of the buffer the program waits on. */
static int offer_one(void)
{
    uint64_t one = 1;

    if (rx_avail_idx != rx_seen) {
        return 0;
    }
    offer_rx((int)(rx_avail_idx % NUM));
    return write(ev[0], &one, 8) == 8 ? 0 : -1;
}

/**
 * Wait for the frame `match` accepts with `text` on the receive ring, one
 * frame at a time: offer a buffer when the ring holds none, read the call,
 * which comes once the buffer's frame is on the ring, settle the count and
 * look at the frame, so that each frame has its own call before the next
 * buffer is offered. Other frames the stack sends are passed over, however
 * many come. The buffer's offset; -1 when a kick fails or the used index is
 * one no device can give; when a read of the call or the count fails, -2 if
 * the frame is on the ring, else -1.
 */
static int wait_rx(int call, int (*match)(const unsigned char *, uint32_t, const char *),
                   const char *text)
{
    int at, r;

    for (;;) {
        if (offer_one() != 0) {
            return -1;
        }
        if (take_call(call) != 0 || settle_calls(call) != 0) {
            return find_rx(match, text, &at) == 1 ? -2 : -1;
        }
        r = find_rx(match, text, &at);
        if (r < 0) {
            return -1;
        }
        if (r == 1) {
            return at;
        }
    }
}

/** Answer the ARP request at `req`, the frame after a receive header, on
 * the transmit ring's entry 0, with a zero virtio header, packet
 * information and padding to the full frame size. */
static void put_answer(const unsigned char *req)
{
    unsigned char *pi = mem + TX_BUF + HDR;
    unsigned char *f = pi + PI_LEN;

    memset(mem + TX_BUF, 0, HDR + PI_LEN + ETH_HDR + MTU);
    pi[2] = 8;
    pi[3] = 6;
    memcpy(f, req + 6, 6);
    memcpy(f + 6, peer, 6);
    f[12] = 8;
    f[13] = 6;
    memcpy(f + 14, req + 14, 8);
    f[21] = 2;
    memcpy(f + 22, peer, 6);
    memcpy(f + 28, req + 38, 4);
    memcpy(f + 32, req + 22, 10);
    f[ETH_HDR + MTU - 1] = 0x5a;
    desc(TX_DESC, 0, TX_BUF, HDR + PI_LEN + ETH_HDR + MTU, 0);
    put16(TX_AVAIL + 4, 0);
    __sync_synchronize();
    put16(TX_AVAIL + 2, 1);
}

/** Listen for ARP on vh0 before the answer is injected. Returns bind's
 * result, or -1 when the socket cannot be made. */
static int listen_arp(void)
{
    struct sockaddr_ll sa;

    arp_sock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_ARP));
    if (arp_sock < 0) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(ETH_ARP);
    sa.sll_ifindex = (int)if_nametoindex("vh0");
    return bind(arp_sock, (struct sockaddr *)&sa, sizeof(sa));
}

/** Check the frame injected before the transmit call was signalled. */
static int full_frame_received(void)
{
    unsigned char payload[MTU];
    const unsigned char *sent = mem + TX_BUF + HDR + PI_LEN + ETH_HDR;

    return recv(arp_sock, payload, sizeof(payload), MSG_DONTWAIT) == sizeof(payload) &&
           memcmp(payload, sent, sizeof(payload)) == 0;
}

/** Close what the program made, which removes the tap. */
static void cleanup(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (ev[i] >= 0) {
            close(ev[i]);
        }
    }
    if (sock >= 0) {
        close(sock);
    }
    if (arp_sock >= 0) {
        close(arp_sock);
    }
    if (vh >= 0) {
        close(vh);
    }
    if (tap >= 0) {
        close(tap);
    }
}

/** Make the tap vh0 at 10.9.0.1/24 and map the guest's memory. */
static int setup_tap(void)
{
    struct ifreq ifr;

    tap = open("/dev/net/tun", O_RDWR);
    if (tap < 0) {
        return fail(TAP_OPEN);
    }
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "vh0");
    ifr.ifr_flags = 0x0002; /* IFF_TAP */
    if (ioctl(tap, TUNSETIFF, &ifr) != 0 || strcmp(ifr.ifr_name, "vh0") != 0) {
        return fail(TAP_IFF);
    }
    if (address_tap() != 0) {
        return fail(TAP_ADDRESS);
    }
    mem = mmap(0, MEMSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return fail(MEMORY);
    }
    memset(mem, 0, MEMSIZE);
    return 0;
}

/** Give /dev/vhost-net the guest's memory and both rings, as QEMU does, up
 * to the tap given to both. */
static int setup_vhost(void)
{
    uint64_t f = 0, table[5];
    int i;

    vh = open("/dev/vhost-net", O_RDWR);
    if (vh < 0) {
        return fail(VHOST_OPEN);
    }
    if (ioctl(vh, VHOST_SET_OWNER, 0) != 0) {
        return fail(OWNER);
    }
    if (ioctl(vh, VHOST_GET_FEATURES, &f) != 0 ||
        (f & (F_NET_HDR | F_VERSION_1)) != (F_NET_HDR | F_VERSION_1)) {
        return fail(FEATURES);
    }
    f = F_NET_HDR | F_VERSION_1;
    if (ioctl(vh, VHOST_SET_FEATURES, &f) != 0) {
        return fail(SET_FEATURES);
    }
    table[0] = 1;
    table[1] = GPA;
    table[2] = MEMSIZE;
    table[3] = (uintptr_t)mem;
    table[4] = 0;
    if (ioctl(vh, VHOST_SET_MEM_TABLE, table) != 0) {
        return fail(MEM_TABLE);
    }
    for (i = 0; i < 4; i++) {
        ev[i] = eventfd(0, EFD_CLOEXEC);
        if (ev[i] < 0) {
            return fail(EVENTFDS);
        }
    }
    if (ring2(vh, VHOST_SET_VRING_NUM, 0, NUM) != 0 ||
        ring2(vh, VHOST_SET_VRING_NUM, 1, NUM) != 0 || ring2(vh, VHOST_SET_VRING_BASE, 0, 0) != 0 ||
        ring2(vh, VHOST_SET_VRING_BASE, 1, 0) != 0 ||
        ring_addr(vh, 0, RX_DESC, RX_USED, RX_AVAIL) != 0 ||
        ring_addr(vh, 1, TX_DESC, TX_USED, TX_AVAIL) != 0) {
        return fail(RINGS);
    }
    if (ring2(vh, VHOST_SET_VRING_KICK, 0, ev[0]) != 0 ||
        ring2(vh, VHOST_SET_VRING_CALL, 0, ev[1]) != 0 ||
        ring2(vh, VHOST_SET_VRING_KICK, 1, ev[2]) != 0 ||
        ring2(vh, VHOST_SET_VRING_CALL, 1, ev[3]) != 0) {
        return fail(RING_FILES);
    }
    if (ring2(vh, VHOST_NET_SET_BACKEND, 0, tap) != 0 ||
        ring2(vh, VHOST_NET_SET_BACKEND, 1, tap) != 0) {
        return fail(BACKEND);
    }
    return 0;
}

/** The ARP request held in the tap until buffers are offered, the answer on
 * the transmit ring, and the datagram that waited for it. */
static int exchange(void)
{
    uint64_t one = 1, v = 0;
    int at;

    sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0 || send_to_peer(sock, "vhost1") != 0) {
        return fail(SEND);
    }
    if (get16(RX_USED + 2) != 0) {
        return fail(HELD);
    }
    if (offer_one() != 0) {
        return fail(RX_KICK);
    }
    at = wait_rx(ev[1], arp_match, 0);
    if (at < 0) {
        return fail(ARP_FRAME);
    }
    if (mem[at] != 0 || mem[at + 1] != 0 || get16(at + 10) != 1 || mem[at + HDR + 2] != 8 ||
        mem[at + HDR + 3] != 6) {
        return fail(ARP_HEADER);
    }
    if (listen_arp() != 0) {
        return fail(TX_KICK);
    }
    put_answer(mem + at + HDR + PI_LEN);
    if (write(ev[2], &one, 8) != 8) {
        return fail(TX_KICK);
    }
    if (read(ev[3], &v, 8) != 8) {
        return fail(TX_CALL);
    }
    if (get16(TX_USED + 2) != 1 || get16(TX_USED + 4) != 0) {
        return fail(TX_USED_ENTRY);
    }
    if (!full_frame_received()) {
        return fail(TX_FULL_FRAME);
    }
    if (wait_rx(ev[1], is_datagram, "vhost1") < 0) {
        return fail(DATAGRAM);
    }
    return 0;
}

/** A datagram sent once a receive buffer is offered comes into one, at once
 * or from the thread, with its own call; then the rings let the tap go,
 * and the calls read equal the used entries. A frame of the stack's
 * waiting in the tap may take the buffer first, and the datagram then
 * comes from the thread into the next. */
static int direct(void)
{
    uint64_t used;
    int at;

    if (offer_one() != 0 || send_to_peer(sock, "vhost2") != 0) {
        return fail(DIRECT_SEND);
    }
    at = wait_rx(ev[1], is_datagram, "vhost2");
    if (at == -2) {
        return fail(DIRECT_CALL);
    }
    if (at < 0) {
        return fail(DIRECT_FRAME);
    }
    if (ring2(vh, VHOST_NET_SET_BACKEND, 0, -1) != 0 ||
        ring2(vh, VHOST_NET_SET_BACKEND, 1, -1) != 0) {
        return fail(DETACH);
    }
    if (drain_calls(ev[1]) != 0 || rx_used(&used) != 0) {
        return fail(DETACH);
    }
    if (rx_calls != used) {
        miscount(used);
        return fail(DETACH);
    }
    return 0;
}

/** Run the checks in order, report them and clean up. */
int main(void)
{
    int rc;

    report_begin(report_case(environ, "guest.default.vhostmusl"));
    rc = setup_tap();
    if (rc == 0) {
        rc = setup_vhost();
    }
    if (rc == 0) {
        rc = exchange();
    }
    if (rc == 0) {
        rc = direct();
    }
    cleanup();
    if (rc == 0) {
        report_at(CHECKS);
        printf("VHOSTMUSL OK\n");
    }
    return rc;
}
