/**
 * frag6probe.c: IPv6 fragments, extension headers and the path MTU over ::1,
 * judged by the replies that come back and by /proc/net/snmp6.
 *
 * The probe sends through the kernel's echo, SC_PING6, through a raw ICMPv6
 * socket, and through a raw socket of IPPROTO_RAW, which sends a datagram it
 * built whole: fragments and extension headers no other path makes. Every
 * datagram goes round the loopback ring, which delivers in the order it was
 * given them, so a probe that sends a crafted datagram and then a plain echo
 * request, and reads its raw socket until that echo's reply, has seen by
 * then whatever answer the crafted one had: an absent answer is judged by
 * that order, never by a wait.
 *
 * Prints "FRAG6PROBE OK" and exits 0 on success, or "FRAG6PROBE FAIL: " and
 * the reason and exits 1.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define AF_INET6       10
#define SOCK_RAW       3
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW    255
#define POLLIN         0x001
#define MSG_DONTWAIT   0x40

#define NH_ROUTING  43
#define NH_FRAGMENT 44
#define NH_ICMPV6   58
#define NH_DSTOPTS  60

#define ICMP6_PACKET_TOO_BIG 2
#define ICMP6_PARAM_PROB     4
#define ICMP6_ECHO_REQUEST   128
#define ICMP6_ECHO_REPLY     129

/* The echo identifiers of this probe's own requests, apart from the
 * kernel's ping. */
#define ID_OPTIONS  0x6601
#define ID_OPTION   0x6602
#define ID_ROUTING  0x6603
#define ID_OVERLAP  0x6604
#define ID_SENTINEL 0x6605

/* The data of the large echo, past one datagram of lo's 65536 bytes; of
 * the echo that fits one but not a 1280-byte path; and the MTU the Packet
 * Too Big gives. */
#define BIG_DATA    65527
#define MID_DATA    1300
#define TOO_BIG_MTU 1280

struct pollfd_probe {
    int fd;
    short events;
    short revents;
};

/* The path, in the order the probe checks it. */
static const char *const path_rows[] = {
    "snmp6",           "fragmented-echo", "fragments-counted", "raw-open",
    "options-skipped", "option-refused",  "routing-refused",   "overlap-dropped",
    "unfragmented",    "too-big-taken",   "too-big-lowers",
};

static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static uint8_t dgram[2048];
static uint8_t msg[2048];
static char text[4096];
static uint8_t sa6[28];

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Write the string `s` to standard output. */
static void say(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    sys_write(1, s, n);
}

/** Print "FRAG6PROBE FAIL: " and `what`, report that the assertion of the
 * path being checked failed, and exit 1. */
static void fail(const char *what)
{
    say("FRAG6PROBE FAIL: ");
    say(what);
    say("\n");
    report_stop(what);
    sys_exit(1);
}

/** The socket operation `call`, an SC_ number, through `sock_call`. */
static int socketcall(int call, unsigned long *args)
{
    return (int)sock_call(call, args);
}

/** The counter `name` of /proc/net/snmp6, or 0xFFFFFFFF when the file or
 * the name is absent: the number after the name, past spaces and a tab. */
static uint32_t snmp6(const char *name)
{
    int fd = (int)syscall3(SYS_OPEN, (long)"/proc/net/snmp6", 0, 0);
    int n = 0, r, i;

    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    while (n < (int)sizeof(text) - 1 &&
           (r = (int)syscall3(SYS_READ, fd, (long)(text + n), (long)(sizeof(text) - 1 - n))) > 0) {
        n += r;
    }
    syscall1(SYS_CLOSE, fd);
    text[n] = '\0';
    for (i = 0; i < n; i++) {
        int k;

        if (i > 0 && text[i - 1] != '\n') {
            continue;
        }
        for (k = 0; name[k] && text[i + k] == name[k]; k++) {
        }
        if (!name[k] && (text[i + k] == ' ' || text[i + k] == '\t')) {
            uint32_t v = 0;

            i += k;
            while (text[i] == ' ' || text[i] == '\t') {
                i++;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (uint32_t)(text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0xFFFFFFFFu;
}

/** The kernel's echo to ::1 with `size` bytes of data: the reply's ICMPv6
 * length, or a negated errno. */
static int ping6(uint32_t size)
{
    unsigned long args[4];
    int r;

    args[0] = (unsigned long)loopback;
    args[1] = 0;
    args[2] = size;
    args[3] = 64;
    r = socketcall(SC_PING6, args);
    return r < 0 ? r : (r & 0xffff);
}

/** Open a raw IPv6 socket of `protocol`: its descriptor, or a negated
 * errno. */
static int raw6_open(int protocol)
{
    unsigned long args[3];

    args[0] = AF_INET6;
    args[1] = SOCK_RAW;
    args[2] = (unsigned long)protocol;
    return socketcall(SC_SOCKET, args);
}

/** Send the `len` bytes at `buf` on `fd` to ::1: the count sent, or a
 * negated errno. */
static int send6(int fd, const uint8_t *buf, int len)
{
    unsigned long args[6];
    int i;

    for (i = 0; i < 28; i++) {
        sa6[i] = 0;
    }
    sa6[0] = AF_INET6;
    for (i = 0; i < 16; i++) {
        sa6[8 + i] = loopback[i];
    }
    args[0] = (unsigned long)fd;
    args[1] = (unsigned long)buf;
    args[2] = (unsigned long)len;
    args[3] = 0;
    args[4] = (unsigned long)sa6;
    args[5] = sizeof(sa6);
    return socketcall(SC_SENDTO, args);
}

/** Wait for an ICMPv6 message on the raw socket `fd` and read it into
 * `msg`: its length, or a negated errno. */
static int recv6(int fd)
{
    struct pollfd_probe pfd;
    unsigned long args[6];

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (syscall3(SYS_POLL, (long)&pfd, 1, -1) <= 0) {
        return -1;
    }
    args[0] = (unsigned long)fd;
    args[1] = (unsigned long)msg;
    args[2] = sizeof(msg);
    args[3] = MSG_DONTWAIT;
    args[4] = 0;
    args[5] = 0;
    return socketcall(SC_RECVFROM, args);
}

/** The 16-bit big-endian word at `p`. */
static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

/** Store `v` big-endian in the 16 bits at `p`. */
static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/**
 * Write at `p` an echo request of `len` bytes, data included, with the
 * identifier `id`, its checksum summed over the pseudo-header of ::1 to
 * ::1, as RFC 8200, 8.1 has it.
 */
static void echo_request(uint8_t *p, int len, uint32_t id)
{
    unsigned long sum = (unsigned long)len + NH_ICMPV6 + 2; /* the two ::1 */
    int i;

    p[0] = ICMP6_ECHO_REQUEST;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    put16(p + 4, id);
    put16(p + 6, 1);
    for (i = 8; i < len; i++) {
        p[i] = (uint8_t)i;
    }
    for (i = 0; i + 1 < len; i += 2) {
        sum += be16(p + i);
    }
    if (i < len) {
        sum += (unsigned long)p[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    put16(p + 2, ~sum & 0xffff);
}

/** Write at `dgram` a fixed header from ::1 to ::1 with `plen` bytes of
 * payload and the next header `nh`. */
static void header(uint32_t plen, uint8_t nh)
{
    int i;

    dgram[0] = 0x60;
    dgram[1] = 0;
    dgram[2] = 0;
    dgram[3] = 0;
    put16(dgram + 4, plen);
    dgram[6] = nh;
    dgram[7] = 64;
    for (i = 0; i < 16; i++) {
        dgram[8 + i] = loopback[i];
        dgram[24 + i] = loopback[i];
    }
}

/**
 * Send on `whole`, the IPPROTO_RAW socket, an echo request of identifier
 * `id` behind an 8-byte extension header of next-header value `nh`, whose
 * last six bytes are `ext`.
 */
static void send_behind(int whole, uint8_t nh, const uint8_t *ext, uint32_t id)
{
    int i;

    header(8 + 16, nh);
    dgram[40] = NH_ICMPV6;
    dgram[41] = 0;
    for (i = 0; i < 6; i++) {
        dgram[42 + i] = ext[i];
    }
    echo_request(dgram + 48, 16, id);
    if (send6(whole, dgram, 64) != 64) {
        fail("a datagram built whole did not send");
    }
}

/**
 * Send on `whole` the fragment of offset `off`, in bytes, with the
 * more-fragments flag `more`, of the datagram of identification `ident`,
 * carrying the `len` bytes at `data`.
 */
static void send_fragment(int whole, uint32_t ident, uint32_t off, int more, const uint8_t *data,
                          int len)
{
    int i;

    header((uint32_t)(8 + len), NH_FRAGMENT);
    dgram[40] = NH_ICMPV6;
    dgram[41] = 0;
    put16(dgram + 42, off | (more ? 1u : 0u));
    dgram[44] = (uint8_t)(ident >> 24);
    dgram[45] = (uint8_t)(ident >> 16);
    dgram[46] = (uint8_t)(ident >> 8);
    dgram[47] = (uint8_t)ident;
    for (i = 0; i < len; i++) {
        dgram[48 + i] = data[i];
    }
    if (send6(whole, dgram, 48 + len) != 48 + len) {
        fail("a fragment built whole did not send");
    }
}

/**
 * Send on `icmp`, the raw ICMPv6 socket, an echo request of the sentinel
 * identifier, and read the socket until its reply. Returns the type and
 * code of the last error read before it, type << 8 | code, with its pointer
 * in `*pointer`, or 0 for none; and in `*answered` whether an echo reply of
 * the identifier `other` came first.
 */
static uint32_t until_sentinel(int icmp, uint32_t other, int *answered, uint32_t *pointer)
{
    uint32_t error = 0;
    int n;

    echo_request(msg, 16, ID_SENTINEL);
    if (send6(icmp, msg, 16) != 16) {
        fail("the sentinel echo did not send");
    }
    *answered = 0;
    for (;;) {
        n = recv6(icmp);
        if (n < 8) {
            fail("a read of the raw ICMPv6 socket failed");
        }
        if (msg[0] == ICMP6_ECHO_REPLY && be16(msg + 4) == ID_SENTINEL) {
            return error;
        }
        if (msg[0] == ICMP6_ECHO_REPLY && be16(msg + 4) == other) {
            *answered = 1;
        }
        if (msg[0] < 128) {
            error = (uint32_t)msg[0] << 8 | msg[1];
            *pointer = ((uint32_t)msg[4] << 24) | ((uint32_t)msg[5] << 16) |
                       ((uint32_t)msg[6] << 8) | msg[7];
        }
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, in the order of `path_rows`; it fails at the first wrong
 * answer.
 *
 * An echo of 65527 bytes of data, the most an IPv6 datagram holds, leaves
 * in two fragments each way, since with its headers it is 39 bytes past lo's
 * MTU of 65536, and must come back whole: two datagrams fragmented, four
 * fragments made and two reassembled.
 *
 * A destination options header holding only PadN is skipped, and the echo
 * behind it answered. One holding an option of type 0x80, unknown and of
 * action 10, is answered with a parameter problem of code 2 pointing at the
 * option, byte 42; a routing header of type 0 with a segment left, with one
 * of code 0 pointing at the routing type, byte 42 too (RFC 5095).
 *
 * The overlap: an echo request of 24 bytes in fragments of identification
 * 0x6604 at offsets 0 (16 bytes) and 8 (16 bytes), which share bytes 8 to
 * 15. RFC 5722 has the whole datagram dropped, the fragments still to come
 * with it, so a third fragment at offset 16, which with the first would
 * make the datagram whole, must bring no reply either; the reassembly is
 * counted as failed.
 *
 * The path MTU: an echo of 1300 bytes of data fits one datagram of lo's MTU
 * and leaves whole. A Packet Too Big to ::1 that quotes a datagram from ::1
 * to ::1 and reports 1280 bytes lowers the path to ::1, so the same echo
 * then leaves in fragments each way. The lowered path stays ten minutes,
 * for the rest of the boot's cases too.
 */
void probe_main(unsigned long *sp)
{
    static const uint8_t padn[6] = {1, 4, 0, 0, 0, 0};
    static const uint8_t unknown[6] = {0x80, 4, 0, 0, 0, 0};
    static const uint8_t segment[6] = {0, 1, 0, 0, 0, 0};
    uint32_t frag_oks, frag_creates, reasm_oks, reasm_fails, too_bigs, error, pointer = 0;
    unsigned int k;
    int icmp, whole, answered, r;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 2 + sp[0]), "guest.frag6.frag6probe"));
    for (k = 0; k < sizeof(path_rows) / sizeof(path_rows[0]); k++) {
        report_plan(path_rows[k]);
    }

    frag_oks = snmp6("Ip6FragOKs");
    frag_creates = snmp6("Ip6FragCreates");
    reasm_oks = snmp6("Ip6ReasmOKs");
    reasm_fails = snmp6("Ip6ReasmFails");
    too_bigs = snmp6("Icmp6InPktTooBigs");
    if (frag_oks == 0xFFFFFFFFu || reasm_fails == 0xFFFFFFFFu || too_bigs == 0xFFFFFFFFu) {
        fail("/proc/net/snmp6 did not read with the fragment counts");
    }
    report_held();

    r = ping6(BIG_DATA);
    if (r != 8 + BIG_DATA) {
        fail("an echo of 65527 bytes over ::1 did not come back whole");
    }
    report_held();
    if (snmp6("Ip6FragOKs") != frag_oks + 2 || snmp6("Ip6FragCreates") != frag_creates + 4 ||
        snmp6("Ip6ReasmOKs") != reasm_oks + 2) {
        fail("the echo was not sent as two datagrams of two fragments, each reassembled");
    }
    report_held();

    icmp = raw6_open(IPPROTO_ICMPV6);
    whole = raw6_open(IPPROTO_RAW);
    if (icmp < 0 || whole < 0) {
        fail("a raw ICMPv6 socket or one of IPPROTO_RAW did not open");
    }
    report_held();

    send_behind(whole, NH_DSTOPTS, padn, ID_OPTIONS);
    until_sentinel(icmp, ID_OPTIONS, &answered, &pointer);
    if (!answered) {
        fail("an echo behind a destination options header of padding was not answered");
    }
    report_held();

    send_behind(whole, NH_DSTOPTS, unknown, ID_OPTION);
    error = until_sentinel(icmp, ID_OPTION, &answered, &pointer);
    if (answered || error != (ICMP6_PARAM_PROB << 8 | 2) || pointer != 42) {
        fail("an unknown option of action 10 was not refused with a parameter problem at it");
    }
    report_held();

    send_behind(whole, NH_ROUTING, segment, ID_ROUTING);
    error = until_sentinel(icmp, ID_ROUTING, &answered, &pointer);
    if (answered || error != ICMP6_PARAM_PROB << 8 || pointer != 42) {
        fail("a routing header with a segment left was not refused at its type");
    }
    report_held();

    echo_request(msg, 24, ID_OVERLAP);
    reasm_fails = snmp6("Ip6ReasmFails");
    send_fragment(whole, 0x6604, 0, 1, msg, 16);
    send_fragment(whole, 0x6604, 8, 0, msg + 8, 16);
    send_fragment(whole, 0x6604, 16, 0, msg + 16, 8);
    until_sentinel(icmp, ID_OVERLAP, &answered, &pointer);
    if (answered || snmp6("Ip6ReasmFails") != reasm_fails + 1) {
        fail("overlapping fragments were not dropped with the rest of their datagram");
    }
    report_held();

    frag_oks = snmp6("Ip6FragOKs");
    if (ping6(MID_DATA) != 8 + MID_DATA || snmp6("Ip6FragOKs") != frag_oks) {
        fail("an echo of 1300 bytes did not leave whole on lo's 65536-byte path");
    }
    report_held();

    /* The Packet Too Big: its MTU, then the datagram it quotes, a header
     * from ::1 to ::1 and the start of an echo request. */
    msg[0] = ICMP6_PACKET_TOO_BIG;
    msg[1] = 0;
    msg[2] = 0;
    msg[3] = 0;
    msg[4] = 0;
    msg[5] = 0;
    put16(msg + 6, TOO_BIG_MTU);
    header(8 + MID_DATA, NH_ICMPV6);
    for (k = 0; k < 40; k++) {
        msg[8 + k] = dgram[k];
    }
    echo_request(msg + 48, 8, ID_SENTINEL);
    if (send6(icmp, msg, 56) != 56) {
        fail("the Packet Too Big did not send");
    }
    do {
        r = recv6(icmp);
        if (r < 8) {
            fail("a read of the raw ICMPv6 socket failed");
        }
    } while (msg[0] != ICMP6_PACKET_TOO_BIG);
    if (snmp6("Icmp6InPktTooBigs") != too_bigs + 1) {
        fail("the Packet Too Big was not counted");
    }
    report_held();
    if (ping6(MID_DATA) != 8 + MID_DATA || snmp6("Ip6FragOKs") != frag_oks + 2) {
        fail("the echo of 1300 bytes did not leave in fragments on the lowered path");
    }
    report_held();

    report_end();
    say("FRAG6PROBE OK\n");
    sys_exit(0);
}
