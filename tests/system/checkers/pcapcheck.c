/**
 * pcapcheck: read a QEMU packet capture and check what the guest sent.
 *
 * QEMU's filter-dump object writes every frame the guest sends to a libpcap
 * file, so what a machine put on the wire can be judged after the fact,
 * from outside, where nothing in the guest can forge it. Three checks share
 * this one reader:
 *
 * `pcapcheck bcast CAPTURE ...` finds evidence of datagram behavior. Its
 * options compose: `--source A.B.C.D` requires a packet sent from the
 * address (only a machine holding it can), `--owns A.B.C.D` requires an ARP
 * reply claiming it, `--arp-from A.B.C.D` requires an ARP request sent as
 * it, `--routed-via GW --routed-dst D` requires every packet
 * for D to leave for the hardware address GW answers ARP at (which is what
 * a default route means), and `--port N [--dst-ip A.B.C.D]` requires a
 * broadcast UDP datagram for that port.
 *
 * `pcapcheck netconsole CAPTURE [--expect TEXT]... [--reject TEXT]...
 * [--min-frames N] [--print]`
 * holds the kernel's netconsole frames (EtherType 0x88b5, see
 * rach2/sys/network/netconsole.c) to the contract their header states: one
 * nonzero boot id, one source address, sequence numbers from zero with no
 * gaps, a length matching the frame, a checksum summing to zero, and a
 * broadcast destination; then requires each `--expect` string in the
 * reassembled log, which proves the output buffered from before the device
 * existed was carried, not lost.
 *
 * `pcapcheck rawwire CAPTURE` checks the two raw-socket facts invisible
 * from inside the machine: the probe's six sends must leave with the hop
 * limits they were given (ICMP echoes at 1, 2, 3 and datagrams at 4, 5, 6),
 * and the echo request deliberately sent with checksum 0x1234 must cross
 * the wire still wrong, because a raw checksum is the caller's.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_NETCONSOLE 0x88B5
#define IPPROTO_ICMP 1
#define IPPROTO_UDP 17
#define NC_HDR_LEN 16

/* ---- the capture reader ---- */

struct pcap {
    unsigned char *data;
    size_t len;
    size_t off;
    int swap;
};

static uint32_t r32be(const unsigned char *p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 |
           (uint32_t) p[2] << 8 | (uint32_t) p[3];
}
static uint32_t r32le(const unsigned char *p)
{
    return (uint32_t) p[3] << 24 | (uint32_t) p[2] << 16 |
           (uint32_t) p[1] << 8 | (uint32_t) p[0];
}
static uint32_t r16be(const unsigned char *p) { return p[0] << 8 | p[1]; }

/* Open a classic or nanosecond pcap of Ethernet frames; exits with the
   diagnostic on anything else, the way every caller treated the errors. */
static void pcap_open(struct pcap *pc, const char *path, const char *tool)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint32_t link;

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: cannot read %s\n", tool, path);
        exit(2);
    }
    pc->data = malloc((size_t) len + 1);
    if (pc->data == NULL ||
        fread(pc->data, 1, (size_t) len, f) != (size_t) len) {
        fprintf(stderr, "%s: cannot read %s\n", tool, path);
        exit(2);
    }
    fclose(f);
    pc->len = (size_t) len;
    pc->off = 24;
    if (pc->len < 24) {
        fprintf(stderr, "%s: capture is shorter than a pcap file header\n",
                tool);
        exit(2);
    }
    if (r32be(pc->data) == 0xD4C3B2A1u || r32be(pc->data) == 0x4D3CB2A1u)
        pc->swap = 1;       /* little-endian file */
    else if (r32be(pc->data) == 0xA1B2C3D4u || r32be(pc->data) == 0xA1B23C4Du)
        pc->swap = 0;       /* big-endian file */
    else {
        fprintf(stderr, "%s: not a pcap capture (magic %02x%02x%02x%02x)\n",
                tool, pc->data[0], pc->data[1], pc->data[2], pc->data[3]);
        exit(2);
    }
    link = pc->swap ? r32le(pc->data + 20) : r32be(pc->data + 20);
    if (link != 1) {
        fprintf(stderr, "%s: capture link type is %u, not Ethernet\n", tool,
                link);
        exit(2);
    }
}

/* The next frame, or NULL at the end (a truncated record ends the file the
   way the readers always ended it). */
static const unsigned char *pcap_next(struct pcap *pc, uint32_t *caplen)
{
    const unsigned char *frame;

    if (pc->off + 16 > pc->len)
        return NULL;
    *caplen = pc->swap ? r32le(pc->data + pc->off + 8)
                       : r32be(pc->data + pc->off + 8);
    pc->off += 16;
    if (pc->off + *caplen > pc->len)
        return NULL;
    frame = pc->data + pc->off;
    pc->off += *caplen;
    return frame;
}

static void pcap_rewind(struct pcap *pc) { pc->off = 24; }

static int parse_ip4(const char *s, unsigned char out[4])
{
    unsigned a, b, c, d;
    char tail;

    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    out[0] = (unsigned char) a;
    out[1] = (unsigned char) b;
    out[2] = (unsigned char) c;
    out[3] = (unsigned char) d;
    return 1;
}

/* ---- bcast ---- */

/* An Ethernet/IPv4 ARP reply, or NULL. */
/* An Ethernet-over-IPv4 ARP frame with operation `op`, 1 for a request and
   2 for a reply, or NULL. */
static const unsigned char *arp_op(const unsigned char *f, uint32_t n, int op)
{
    const unsigned char *arp = f + 14;

    if (n < 14 + 28 || r16be(f + 12) != ETHERTYPE_ARP)
        return NULL;
    if (arp[0] != 0 || arp[1] != 1 || arp[2] != 8 || arp[3] != 0 ||
        arp[4] != 6 || arp[5] != 4 || arp[6] != 0 || arp[7] != op)
        return NULL;
    return arp;
}

static const unsigned char *arp_reply(const unsigned char *f, uint32_t n)
{
    return arp_op(f, n, 2);
}

/* An ARP request the machine sent for someone else carries its own address
   as the sender, so this is where an address reached ARP. It is what can be
   checked when nothing on the network asks for the machine's address, and
   so nothing makes it answer. */
static int check_arp_from(struct pcap *pc, const char *capture,
                          const char *address)
{
    unsigned char want[4];
    const unsigned char *f;
    uint32_t n;

    parse_ip4(address, want);
    pcap_rewind(pc);
    while ((f = pcap_next(pc, &n)) != NULL) {
        const unsigned char *arp = arp_op(f, n, 1);

        if (arp != NULL && memcmp(arp + 14, want, 4) == 0) {
            printf("bcast_check: ok: the machine asked ARP as %s, so that "
                   "is the address its ARP layer holds\n", address);
            return 0;
        }
    }
    fprintf(stderr, "bcast_check: no ARP request in %s was sent as %s, so "
            "that address never reached the machine's ARP layer\n",
            capture, address);
    return 1;
}

static int check_source(struct pcap *pc, const char *capture,
                        const char *address)
{
    unsigned char want[4];
    const unsigned char *f;
    uint32_t n;
    long seen = 0;

    parse_ip4(address, want);
    pcap_rewind(pc);
    while ((f = pcap_next(pc, &n)) != NULL) {
        if (n < 14 + 20 || r16be(f + 12) != ETHERTYPE_IP)
            continue;
        if (memcmp(f + 14 + 12, want, 4) == 0)
            seen++;
    }
    if (seen == 0) {
        fprintf(stderr, "bcast_check: nothing in %s was sent from %s, so no "
                "machine had that address on an interface\n", capture,
                address);
        return 1;
    }
    printf("bcast_check: ok: %ld packet(s) were sent from %s\n", seen,
           address);
    return 0;
}

static int check_owns(struct pcap *pc, const char *capture,
                      const char *address)
{
    unsigned char want[4];
    const unsigned char *f;
    uint32_t n;

    parse_ip4(address, want);
    pcap_rewind(pc);
    while ((f = pcap_next(pc, &n)) != NULL) {
        const unsigned char *arp = arp_reply(f, n);

        if (arp != NULL && memcmp(arp + 14, want, 4) == 0) {
            printf("bcast_check: ok: the machine answered ARP for %s, so "
                   "the address is on its interface\n", address);
            return 0;
        }
    }
    fprintf(stderr, "bcast_check: nothing answered ARP for %s in %s, so no "
            "machine has that address\n", address, capture);
    return 1;
}

static int check_routed(struct pcap *pc, const char *gateway,
                        const char *destination)
{
    unsigned char gw[4], dst[4];
    unsigned char gw_macs[16][6];
    int n_gw = 0, i;
    unsigned char routed_to[6];
    long routed = 0;
    const unsigned char *f;
    uint32_t n;
    static const unsigned char bcast[6] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    parse_ip4(gateway, gw);
    parse_ip4(destination, dst);
    /* Two passes over the capture, so a datagram sent before the gateway's
       reply was recorded is still judged against the full set of replies.
       The address is learned from the gateway's own ARP reply, not from
       where this machine sent things, so a stack that broadcast everything
       cannot agree with itself. */
    pcap_rewind(pc);
    while ((f = pcap_next(pc, &n)) != NULL) {
        const unsigned char *arp = arp_reply(f, n);

        if (arp != NULL && memcmp(arp + 14, gw, 4) == 0 &&
            memcmp(arp + 8, bcast, 6) != 0) {
            for (i = 0; i < n_gw; i++)
                if (memcmp(gw_macs[i], arp + 8, 6) == 0)
                    break;
            if (i == n_gw && n_gw < 16)
                memcpy(gw_macs[n_gw++], arp + 8, 6);
        }
    }
    pcap_rewind(pc);
    while ((f = pcap_next(pc, &n)) != NULL) {
        if (n < 14 + 20 || r16be(f + 12) != ETHERTYPE_IP)
            continue;
        if (memcmp(f + 14 + 16, dst, 4) == 0) {
            for (i = 0; i < n_gw; i++)
                if (memcmp(gw_macs[i], f, 6) == 0)
                    break;
            if (n_gw > 0 && i == n_gw) {
                memcpy(routed_to, f, 6);
                routed = -1;
                break;
            }
            routed++;
        }
    }
    if (n_gw == 0) {
        fprintf(stderr, "bcast_check: %s never answered an ARP request, so "
                "there is no hardware address to compare against\n", gateway);
        return 1;
    }
    if (routed == 0) {
        fprintf(stderr, "bcast_check: nothing was sent to %s at all\n",
                destination);
        return 1;
    }
    if (routed < 0) {
        fprintf(stderr, "bcast_check: a packet for %s went to "
                "%02x%02x%02x%02x%02x%02x, and the gateway %s answers at "
                "%02x%02x%02x%02x%02x%02x: it was not routed through the "
                "gateway\n", destination, routed_to[0], routed_to[1],
                routed_to[2], routed_to[3], routed_to[4], routed_to[5],
                gateway, gw_macs[0][0], gw_macs[0][1], gw_macs[0][2],
                gw_macs[0][3], gw_macs[0][4], gw_macs[0][5]);
        return 1;
    }
    printf("bcast_check: ok: %ld packet(s) for %s went to "
           "%02x%02x%02x%02x%02x%02x, which is where %s answers ARP\n",
           routed, destination, gw_macs[0][0], gw_macs[0][1], gw_macs[0][2],
           gw_macs[0][3], gw_macs[0][4], gw_macs[0][5], gateway);
    return 0;
}

static int check_broadcast(struct pcap *pc, const char *capture, long port,
                           const char *dst_ip)
{
    unsigned char want[4];
    int have_want = 0;
    const unsigned char *f;
    uint32_t n;
    long found = 0, seen = 0;
    static const unsigned char bcast[6] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (dst_ip != NULL) {
        parse_ip4(dst_ip, want);
        have_want = 1;
    }
    pcap_rewind(pc);
    while ((f = pcap_next(pc, &n)) != NULL) {
        uint32_t ihl;

        if (n < 14 + 20 + 8 || r16be(f + 12) != ETHERTYPE_IP)
            continue;
        ihl = (f[14] & 0x0F) * 4;
        if (ihl < 20 || n < 14 + ihl + 8)
            continue;
        if (f[14 + 9] != IPPROTO_UDP)
            continue;
        if ((long) r16be(f + 14 + ihl + 2) != port)
            continue;
        seen++;
        if (memcmp(f, bcast, 6) != 0)
            continue;
        if (have_want && memcmp(f + 14 + 16, want, 4) != 0)
            continue;
        found++;
    }
    if (found == 0) {
        fprintf(stderr, "bcast_check: no broadcast datagram for port %ld in "
                "%s (%ld datagram(s) for that port went somewhere else)\n",
                port, capture, seen);
        return 1;
    }
    printf("bcast_check: ok: %ld broadcast datagram(s) for port %ld\n",
           found, port);
    return 0;
}

static int cmd_bcast(int argc, char **argv)
{
    const char *capture = NULL, *dst_ip = NULL, *routed_via = NULL;
    const char *routed_dst = NULL, *source = NULL, *owns = NULL;
    const char *arp_from = NULL;
    long port = -1;
    struct pcap pc;
    int i, rc;
    unsigned char scratch[4];

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atol(argv[++i]);
        else if (strcmp(argv[i], "--dst-ip") == 0 && i + 1 < argc)
            dst_ip = argv[++i];
        else if (strcmp(argv[i], "--routed-via") == 0 && i + 1 < argc)
            routed_via = argv[++i];
        else if (strcmp(argv[i], "--routed-dst") == 0 && i + 1 < argc)
            routed_dst = argv[++i];
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            source = argv[++i];
        else if (strcmp(argv[i], "--owns") == 0 && i + 1 < argc)
            owns = argv[++i];
        else if (strcmp(argv[i], "--arp-from") == 0 && i + 1 < argc)
            arp_from = argv[++i];
        else if (argv[i][0] != '-' && capture == NULL)
            capture = argv[i];
        else {
            fprintf(stderr, "bcast_check: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (capture == NULL) {
        fprintf(stderr, "usage: pcapcheck bcast CAPTURE [--port N] "
                "[--dst-ip A.B.C.D] [--routed-via GW --routed-dst A.B.C.D] "
                "[--source A.B.C.D] [--owns A.B.C.D] "
                "[--arp-from A.B.C.D]\n");
        return 2;
    }
    if (port < 0 && routed_via == NULL && routed_dst == NULL &&
        owns == NULL && source == NULL && arp_from == NULL) {
        fprintf(stderr, "bcast_check: nothing to check: give --port, or "
                "--routed-via with --routed-dst\n");
        return 2;
    }
    if ((source && !parse_ip4(source, scratch)) ||
        (owns && !parse_ip4(owns, scratch)) ||
        (arp_from && !parse_ip4(arp_from, scratch)) ||
        (routed_via && !parse_ip4(routed_via, scratch)) ||
        (routed_dst && !parse_ip4(routed_dst, scratch)) ||
        (dst_ip && !parse_ip4(dst_ip, scratch))) {
        fprintf(stderr, "bcast_check: a malformed IPv4 address was given\n");
        return 2;
    }
    pcap_open(&pc, capture, "bcast_check");
    if (source != NULL && (rc = check_source(&pc, capture, source)) != 0)
        return rc;
    if (owns != NULL && (rc = check_owns(&pc, capture, owns)) != 0)
        return rc;
    if (arp_from != NULL &&
        (rc = check_arp_from(&pc, capture, arp_from)) != 0)
        return rc;
    if (routed_via != NULL || routed_dst != NULL) {
        if (routed_via == NULL || routed_dst == NULL) {
            fprintf(stderr, "bcast_check: --routed-via and --routed-dst go "
                    "together\n");
            return 2;
        }
        if ((rc = check_routed(&pc, routed_via, routed_dst)) != 0)
            return rc;
    }
    if (port < 0)
        return 0;
    return check_broadcast(&pc, capture, port, dst_ip);
}

/* ---- netconsole ---- */

/* The IP header checksum over `buf`; zero means the frame is intact. */
static unsigned ones_complement_sum(const unsigned char *buf, size_t len)
{
    unsigned long total = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        total += (unsigned long) (buf[i] << 8 | buf[i + 1]);
        if (total > 0xFFFF)
            total -= 0xFFFF;
    }
    if (len & 1) {
        total += (unsigned long) buf[len - 1] << 8;
        if (total > 0xFFFF)
            total -= 0xFFFF;
    }
    return (unsigned) (~total & 0xFFFF);
}

static int nc_fails;

static void nc_fail(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "netconsole_check: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    nc_fails++;
}

static int cmd_netconsole(int argc, char **argv)
{
    const char *capture = NULL;
    const char *expect[32];
    const char *reject[32];
    int n_expect = 0, n_reject = 0, show = 0;
    long min_frames = 1;
    struct pcap pc;
    const unsigned char *f;
    uint32_t n;
    long total = 0, ours = 0, index = -1;
    unsigned char *log = NULL;
    size_t log_len = 0, log_cap = 0;
    uint32_t boot_id = 0;
    int boot_ids = 0, boot_zero = 0, boot_many = 0;
    unsigned char src_mac[6];
    int src_macs = 0, src_many = 0;
    long expect_seq = 0;
    int seq_broken = 0;
    int i;
    static const unsigned char bcastmac[6] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
            if (n_expect < 32)
                expect[n_expect++] = argv[++i];
        } else if (strcmp(argv[i], "--reject") == 0 && i + 1 < argc) {
            if (n_reject < 32)
                reject[n_reject++] = argv[++i];
        } else if (strcmp(argv[i], "--min-frames") == 0 && i + 1 < argc)
            min_frames = atol(argv[++i]);
        else if (strcmp(argv[i], "--print") == 0)
            show = 1;
        else if (argv[i][0] != '-' && capture == NULL)
            capture = argv[i];
        else {
            fprintf(stderr, "netconsole_check: unknown argument %s\n",
                    argv[i]);
            return 2;
        }
    }
    if (capture == NULL) {
        fprintf(stderr, "usage: pcapcheck netconsole CAPTURE "
                "[--expect TEXT ...] [--reject TEXT ...] [--min-frames N] "
                "[--print]\n");
        return 2;
    }
    pcap_open(&pc, capture, "netconsole_check");
    nc_fails = 0;
    while ((f = pcap_next(&pc, &n)) != NULL) {
        const unsigned char *body;
        uint32_t id, seq, length;

        total++;
        if (n < 14 || r16be(f + 12) != ETHERTYPE_NETCONSOLE)
            continue;
        ours++;
        index++;
        body = f + 14;
        if (n - 14 < NC_HDR_LEN) {
            nc_fail("frame %ld is shorter than the netconsole header",
                    index);
            continue;
        }
        if (memcmp(body, "RCH2", 4) != 0) {
            nc_fail("frame %ld does not carry the magic", index);
            continue;
        }
        id = r32be(body + 4);
        seq = r32be(body + 8);
        length = r16be(body + 12);
        /* A frame the chip padded to the 60-byte minimum has trailing
           zeros past the payload; the length field, not the frame size,
           is the log. */
        if (NC_HDR_LEN + length > n - 14) {
            nc_fail("frame %ld claims %u bytes but has %u", index, length,
                    n - 14 - NC_HDR_LEN);
            continue;
        }
        if (ones_complement_sum(body, NC_HDR_LEN + length) != 0)
            nc_fail("frame %ld fails its checksum", index);
        if (memcmp(f, bcastmac, 6) != 0)
            nc_fail("frame %ld is not broadcast", index);
        if (src_macs == 0) {
            memcpy(src_mac, f + 6, 6);
            src_macs = 1;
        } else if (memcmp(src_mac, f + 6, 6) != 0)
            src_many = 1;
        if (boot_ids == 0) {
            boot_id = id;
            boot_ids = 1;
        } else if (id != boot_id)
            boot_many = 1;
        if (id == 0)
            boot_zero = 1;
        if ((long) seq != expect_seq)
            seq_broken = 1;
        expect_seq++;
        if (log_len + length + 1 > log_cap) {
            log_cap = (log_len + length + 1) * 2;
            log = realloc(log, log_cap);
            if (log == NULL) {
                fprintf(stderr, "netconsole_check: out of memory\n");
                return 2;
            }
        }
        memcpy(log + log_len, body + NC_HDR_LEN, length);
        log_len += length;
    }
    if (ours < min_frames)
        nc_fail("only %ld netconsole frames in %ld captured, wanted %ld",
                ours, total, min_frames);
    if (boot_many)
        nc_fail("frames carry more than one boot id");
    if (boot_zero)
        nc_fail("a frame has a zero boot id");
    if (src_many)
        nc_fail("frames carry more than one source address");
    if (seq_broken)
        nc_fail("sequence numbers are not 0..%ld in order", expect_seq - 1);
    if (log == NULL)
        log = calloc(1, 1);
    log[log_len] = '\0';
    for (i = 0; i < n_expect; i++) {
        /* memmem by hand: the log may hold NUL bytes. */
        size_t want_len = strlen(expect[i]);
        size_t j;
        int found = 0;

        for (j = 0; want_len > 0 && j + want_len <= log_len; j++) {
            if (memcmp(log + j, expect[i], want_len) == 0) {
                found = 1;
                break;
            }
        }
        if (!found)
            nc_fail("the log does not contain '%s'", expect[i]);
    }
    /* Text that must NOT have reached the wire. A machine whose card is
       stopped part way through shutting down sends the lines before that
       point and none after it, and only a capture can tell the difference
       between "not sent" and "sent and missed". */
    for (i = 0; i < n_reject; i++) {
        size_t want_len = strlen(reject[i]);
        size_t j;

        for (j = 0; want_len > 0 && j + want_len <= log_len; j++) {
            if (memcmp(log + j, reject[i], want_len) == 0) {
                nc_fail("the log contains '%s', which must not reach the wire",
                        reject[i]);
                break;
            }
        }
    }
    if (show)
        fwrite(log, 1, log_len, stdout);
    if (nc_fails)
        return 1;
    fprintf(stderr, "netconsole_check: ok: %ld frames, %lu bytes, boot id "
            "0x%08x, from %02x:%02x:%02x:%02x:%02x:%02x\n", ours,
            (unsigned long) log_len, boot_id, src_mac[0], src_mac[1],
            src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
    return 0;
}

/* ---- rawwire ---- */

static int cmd_rawwire(int argc, char **argv)
{
    struct pcap pc;
    const unsigned char *f;
    uint32_t n;
    int icmp_ttls[256] = { 0 }, udp_ttls[256] = { 0 };
    int kept_checksum = 0, ok = 1, i;

    if (argc != 1) {
        fprintf(stderr, "usage: pcapcheck rawwire <capture.pcap>\n");
        return 2;
    }
    pcap_open(&pc, argv[0], "rawwire_check");
    while ((f = pcap_next(&pc, &n)) != NULL) {
        uint32_t ihl, body;
        unsigned ttl, proto;

        if (n < 34 || r16be(f + 12) != ETHERTYPE_IP)
            continue;
        ihl = (f[14] & 0x0F) * 4;
        ttl = f[22];
        proto = f[23];
        body = 14 + ihl;
        if (proto == IPPROTO_ICMP && n >= body + 8) {
            if (f[body] == 8) {  /* echo request: only this machine sends */
                icmp_ttls[ttl] = 1;
                if (f[body + 2] == 0x12 && f[body + 3] == 0x34)
                    kept_checksum = 1;
            }
        } else if (proto == IPPROTO_UDP && n >= body + 8) {
            unsigned dport = r16be(f + body + 2);

            if (dport >= 40000 && dport <= 40010)
                udp_ttls[ttl] = 1;
        }
    }
    if (!icmp_ttls[1] || !icmp_ttls[2] || !icmp_ttls[3]) {
        fprintf(stderr, "rawwire_check: FAIL: the raw sends did not carry "
                "the hop limits they were given: saw");
        for (i = 0; i < 256; i++)
            if (icmp_ttls[i])
                fprintf(stderr, " %d", i);
        fputc('\n', stderr);
        ok = 0;
    } else
        printf("rawwire_check: ok: raw sends left with hop limits 1, 2 "
               "and 3\n");
    if (!udp_ttls[4] || !udp_ttls[5] || !udp_ttls[6]) {
        fprintf(stderr, "rawwire_check: FAIL: the datagram sends did not "
                "carry the hop limits they were given: saw");
        for (i = 0; i < 256; i++)
            if (udp_ttls[i])
                fprintf(stderr, " %d", i);
        fputc('\n', stderr);
        ok = 0;
    } else
        printf("rawwire_check: ok: datagram sends left with hop limits 4, "
               "5 and 6\n");
    if (!kept_checksum) {
        fprintf(stderr, "rawwire_check: FAIL: the message with a wrong "
                "checksum did not leave with it, so the checksum is not "
                "the caller's\n");
        ok = 0;
    } else
        printf("rawwire_check: ok: the caller's checksum crossed the wire "
               "unchanged\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "bcast") == 0)
        return cmd_bcast(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "netconsole") == 0)
        return cmd_netconsole(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "rawwire") == 0)
        return cmd_rawwire(argc - 2, argv + 2);
    fprintf(stderr, "usage: pcapcheck {bcast|netconsole|rawwire} "
            "CAPTURE ...\n");
    return 2;
}
