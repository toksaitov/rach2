/**
 * Userspace stub resolver for the Rach2 network utilities, implementing
 * `netdb.h`.
 *
 * `resolve_host` tries, in order, a numeric literal, the `/hosts` file, then a
 * DNS A query to the nameserver in `/resolv.conf`. The pure helpers
 * (`rach2_inet_aton`, `hosts_find`, `dns_build_query`, `dns_parse_a`) take
 * buffers and touch no syscalls, so the `dnstest` program can exercise them
 * directly; only `resolve_host` does file and socket I/O.
 *
 * Only the functions declared in `netdb.h` are external; every other symbol is
 * static, so this object links cleanly into each program that needs it (ping,
 * nc, and the dnstest test). Its file and socket I/O goes through the ported
 * musl, the same libc the rest of the utilities link.
 */

#include "netdb.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* Read from the KRFFS root (not /etc), where the build scripts stage them. */
#define RESOLV_CONF "/resolv.conf"
#define HOSTS_PATH  "/hosts"

/* SLIRP's built-in DNS forwarder, used when /resolv.conf names no server. */
#define DNS_DEFAULT_NS 0x0A000203U   /* 10.0.2.3 */
#define DNS_PORT       53

/** Case-insensitive equality of two NUL-terminated strings. */
static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) {
            return 0;
        }
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/** Exact equality of two NUL-terminated strings. */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/**
 * Read up to `cap` bytes of `path` into `buf`, returning the byte count or -1
 * if the file cannot be opened.
 */
static int read_file(const char *path, char *buf, int cap)
{
    int fd, n, total = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    while (total < cap) {
        n = (int)read(fd, buf + total, (size_t)(cap - total));
        if (n <= 0) {
            break;
        }
        total += n;
    }
    close(fd);
    return total;
}

/**
 * Busy-wait roughly `ms` milliseconds using gettimeofday (the 100 Hz clock).
 */
static void delay_ms(int ms)
{
    struct timeval start, now;
    long elapsed;

    gettimeofday(&start, NULL);
    do {
        gettimeofday(&now, NULL);
        elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                  (now.tv_usec - start.tv_usec) / 1000;
    } while (elapsed < ms);
}

/**
 * Parse a dotted-quad literal in `s` into the host-order IPv4 address `out_ip`.
 *
 * Accepts exactly four octets each in 0..255. Returns 0 on success, -1 on any
 * malformed input.
 */
int rach2_inet_aton(const char *s, unsigned int *out_ip)
{
    u32 ip = 0;
    int octet = 0, parts = 0, digits = 0;

    while (*s) {
        if (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (*s - '0');
            if (octet > 255) {
                return -1;
            }
            digits = 1;
        } else if (*s == '.') {
            if (!digits) {
                return -1;
            }
            ip = (ip << 8) | (u32)octet;
            octet = 0;
            digits = 0;
            if (++parts > 3) {
                return -1;
            }
        } else {
            return -1;
        }
        s++;
    }
    if (!digits || parts != 3) {
        return -1;
    }
    *out_ip = (ip << 8) | (u32)octet;
    return 0;
}

/**
 * Look `name` up in the `/hosts` text held in `buf[0:len]`, returning its
 * address in `out_ip`.
 *
 * Each line is `<address> <name> [name ...]` with `#` starting a comment; the
 * match is case-insensitive on the names. A line whose first token is not a
 * valid address is skipped. Returns 0 on a hit, -1 if `name` is absent.
 */
int hosts_find(const char *buf, int len, const char *name, unsigned int *out_ip)
{
    int i = 0;

    while (i < len) {
        int le, j, ntok = 0;
        u32 ip = 0;

        le = i;
        while (le < len && buf[le] != '\n') {
            le++;
        }

        j = i;
        while (j < le) {
            char tok[64];
            int t = 0;

            while (j < le && (buf[j] == ' ' || buf[j] == '\t')) {
                j++;
            }
            if (j >= le || buf[j] == '#') {
                break;
            }
            while (j < le && buf[j] != ' ' && buf[j] != '\t' && buf[j] != '#') {
                if (t < 63) {
                    tok[t++] = buf[j];
                }
                j++;
            }
            tok[t] = '\0';

            if (ntok == 0) {
                /* first token is the address; a bad one voids the line */
                if (rach2_inet_aton(tok, &ip) != 0) {
                    break;
                }
            } else if (ci_eq(tok, name)) {
                *out_ip = ip;
                return 0;
            }
            ntok++;
        }

        i = (le < len) ? le + 1 : le;
    }
    return -1;
}

/**
 * Encode an A-record query for `name` with transaction `id` into `buf[0:cap]`,
 * in the DNS wire format (RFC 1035).
 *
 * Writes the 12-byte header (RD set, one question) followed by the QNAME as
 * length-prefixed labels and the QTYPE/QCLASS trailer. Returns the byte length
 * of the message, or -1 if `name` is empty, a label is empty or over 63 bytes,
 * or the message would exceed `cap`.
 */
int dns_build_query(unsigned char *buf, int cap, const char *name,
                    unsigned short id)
{
    int pos;
    const char *p = name;

    if (cap < 12 || *name == '\0') {
        return -1;
    }

    /* 12-byte header: id, flags RD=1, one question, no answers. */
    buf[0]  = (u8)(id >> 8); buf[1] = (u8)id;
    buf[2]  = 0x01; buf[3]  = 0x00;   /* QR=0, RD=1 */
    buf[4]  = 0x00; buf[5]  = 0x01;   /* QDCOUNT = 1 */
    buf[6]  = 0x00; buf[7]  = 0x00;   /* ANCOUNT */
    buf[8]  = 0x00; buf[9]  = 0x00;   /* NSCOUNT */
    buf[10] = 0x00; buf[11] = 0x00;   /* ARCOUNT */
    pos = 12;

    /* QNAME: one length-prefixed label per dot-separated component. */
    while (*p) {
        const char *start = p;
        int label = 0, k;

        while (*p && *p != '.') {
            p++; label++;
        }
        if (label == 0 || label > 63) {
            return -1;             /* empty or oversize label */
        }
        if (pos + 1 + label + 1 + 4 > cap) {
            return -1;
        }
        buf[pos++] = (u8)label;
        for (k = 0; k < label; k++) {
            buf[pos++] = (u8)start[k];
        }
        if (*p == '.') {
            p++;
        }
    }

    buf[pos++] = 0x00;             /* root label terminates QNAME */
    buf[pos++] = 0x00; buf[pos++] = 0x01;   /* QTYPE  = A  */
    buf[pos++] = 0x00; buf[pos++] = 0x01;   /* QCLASS = IN */
    return pos;
}

/**
 * Advance past a domain name at offset `pos` and return the position just after
 * it.
 *
 * Length-prefixed labels are walked; a 0x00 byte or a 0xC0 compression pointer
 * (two bytes) ends the name. Pointers are not followed, since skipping is
 * enough to reach the fixed fields, so this cannot loop. Returns -1 on a
 * truncated or reserved-type name.
 */
static int dns_skip_name(const u8 *resp, int len, int pos)
{
    while (pos < len) {
        u8 b = resp[pos];

        if (b == 0x00) {
            return pos + 1;
        }
        if ((b & 0xC0) == 0xC0) {
            return (pos + 2 <= len) ? pos + 2 : -1;
        }
        if ((b & 0xC0) != 0) {
            return -1;                      /* reserved label type */
        }
        pos += 1 + b;                       /* length-prefixed label */
    }
    return -1;
}

/**
 * Extract the first A record from the DNS response `resp[0:len]` into `out_ip`.
 *
 * The response must match `id`, be a reply (QR set) with RCODE 0 and at least
 * one answer. The question section is skipped, then the answers are walked for
 * an IN A record with a 4-byte RDATA. Returns 0 with `out_ip` set in host
 * order, or -1 on a mismatch, error response, or absence of an A record.
 */
int dns_parse_a(const unsigned char *resp, int len, unsigned short id,
                unsigned int *out_ip)
{
    int pos, i, qd, an;

    if (len < 12) {
        return -1;
    }
    if (((resp[0] << 8) | resp[1]) != (int)id) {
        return -1;                          /* not our query */
    }
    if ((resp[2] & 0x80) == 0) {
        return -1;                          /* not a response */
    }
    if ((resp[3] & 0x0F) != 0) {
        return -1;                          /* RCODE != 0 (e.g. NXDOMAIN) */
    }
    qd = (resp[4] << 8) | resp[5];
    an = (resp[6] << 8) | resp[7];
    if (an < 1) {
        return -1;                          /* no answers */
    }

    pos = 12;
    for (i = 0; i < qd; i++) {               /* skip the question section */
        pos = dns_skip_name(resp, len, pos);
        if (pos < 0 || pos + 4 > len) {
            return -1;
        }
        pos += 4;                            /* QTYPE + QCLASS */
    }

    for (i = 0; i < an; i++) {               /* walk the answers */
        int type, cls, rdlen;

        pos = dns_skip_name(resp, len, pos);
        if (pos < 0 || pos + 10 > len) {
            return -1;
        }
        type  = (resp[pos + 0] << 8) | resp[pos + 1];
        cls   = (resp[pos + 2] << 8) | resp[pos + 3];
        rdlen = (resp[pos + 8] << 8) | resp[pos + 9];
        pos += 10;                           /* TYPE CLASS TTL RDLENGTH */
        if (pos + rdlen > len) {
            return -1;
        }
        if (type == 1 && cls == 1 && rdlen == 4) {
            *out_ip = ((u32)resp[pos + 0] << 24) | ((u32)resp[pos + 1] << 16) |
                      ((u32)resp[pos + 2] << 8)  |  (u32)resp[pos + 3];
            return 0;
        }
        pos += rdlen;                        /* CNAME or other record */
    }
    return -1;                               /* no A record present */
}

/**
 * Return the first `nameserver <ip>` from `/resolv.conf`, or `DNS_DEFAULT_NS`
 * (SLIRP's built-in forwarder) when the file names none.
 */
static u32 read_nameserver(void)
{
    char buf[256];
    int n, i;

    n = read_file(RESOLV_CONF, buf, (int)sizeof(buf));
    i = 0;
    while (i < n) {
        char key[16], val[64];
        int le, j, t;

        le = i;
        while (le < n && buf[le] != '\n') {
            le++;
        }
        j = i;
        while (j < le && (buf[j] == ' ' || buf[j] == '\t')) {
            j++;
        }
        t = 0;
        while (j < le && buf[j] != ' ' && buf[j] != '\t') {
            if (t < 15) key[t++] = buf[j];
            j++;
        }
        key[t] = '\0';
        if (str_eq(key, "nameserver")) {
            while (j < le && (buf[j] == ' ' || buf[j] == '\t')) {
                j++;
            }
            t = 0;
            while (j < le && buf[j] != ' ' && buf[j] != '\t') {
                if (t < 63) val[t++] = buf[j];
                j++;
            }
            val[t] = '\0';
            {
                u32 ns;
                if (rach2_inet_aton(val, &ns) == 0) {
                    return ns;
                }
            }
        }
        i = (le < n) ? le + 1 : le;
    }
    return DNS_DEFAULT_NS;
}

/**
 * Resolve `name` to an A record by querying `ns` over UDP/53, returning the
 * address in `out_ip`.
 *
 * Sends the query and polls for the reply with `MSG_DONTWAIT`, resending
 * periodically: the first datagram to the on-subnet nameserver is dropped
 * while ARP resolves, so a resend is expected. A reply whose id matches is
 * decisive (success, NXDOMAIN, or no A record); other traffic is ignored
 * until the roughly 6s budget expires.
 */
static int dns_query(const char *name, u32 ns, unsigned int *out_ip)
{
    u8 query[300], resp[512];
    struct sockaddr_in sa = {0};
    struct timeval tv;
    int fd, qlen, attempt, poll, n, rc = -1, got = 0;
    u16 id;

    gettimeofday(&tv, NULL);
    id = (u16)(((u32)tv.tv_usec ^ ((u32)tv.tv_usec >> 7) ^ (u32)tv.tv_sec) & 0xFFFF);

    qlen = dns_build_query(query, (int)sizeof(query), name, id);
    if (qlen < 0) {
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    sa.sin_family = AF_INET;
    sa.sin_port = htons(DNS_PORT);
    sa.sin_addr.s_addr = htonl(ns);

    for (attempt = 0; attempt < 30 && !got; attempt++) {
        sendto(fd, query, (size_t)qlen, 0, (struct sockaddr *)&sa, sizeof(sa));

        for (poll = 0; poll < 4 && !got; poll++) {
            delay_ms(50);
            n = (int)recvfrom(fd, resp, sizeof(resp), MSG_DONTWAIT, NULL, NULL);
            if (n >= 12 && ((resp[0] << 8) | resp[1]) == (int)id &&
                (resp[2] & 0x80)) {
                rc = dns_parse_a(resp, n, id, out_ip);
                got = 1;
            }
        }
    }

    close(fd);
    return got ? rc : -1;
}

/**
 * Resolve `host` to a host-order IPv4 address in `out_ip`.
 *
 * Tries, in order, a numeric dotted-quad literal, the `/hosts` file, then a
 * DNS A query to the `/resolv.conf` nameserver. Returns 0 on the first hit,
 * or the DNS query's result if neither earlier source matches.
 */
int resolve_host(const char *host, unsigned int *out_ip)
{
    char fbuf[1024];
    int n;

    if (rach2_inet_aton(host, out_ip) == 0) {
        return 0;
    }

    n = read_file(HOSTS_PATH, fbuf, (int)sizeof(fbuf));
    if (n > 0 && hosts_find(fbuf, n, host, out_ip) == 0) {
        return 0;
    }

    return dns_query(host, read_nameserver(), out_ip);
}
