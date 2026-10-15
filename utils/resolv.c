/**
 * The stub resolver of the network utilities; netdb.h has the interface.
 *
 * `rach2_inet_aton`, `hosts_find`, `dns_build_query` and `dns_parse_a` work
 * on buffers and make no system call, so the dnstest probe and the host test
 * drive them directly; only `resolve_host` reads files and uses a socket,
 * through the ported musl the other utilities link. Every other symbol is
 * static, so this object links into any program that needs it.
 */

#include "netdb.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/* Each file under `/etc`, where os-simple and os-real stage it, and at the
 * root, where the test image stages it; `read_either` tries them in that
 * order, as `adduser` looks for the password program. */
#define RESOLV_CONF     "/etc/resolv.conf"
#define RESOLV_CONF_ALT "/resolv.conf"
#define HOSTS_PATH      "/etc/hosts"
#define HOSTS_PATH_ALT  "/hosts"

/* SLIRP's built-in DNS forwarder, used when the resolver config lists none. */
#define DNS_DEFAULT_NS 0x0A000203U /* 10.0.2.3 */
#define DNS_PORT       53

/** Case-insensitive equality of two NUL-terminated strings. */
static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
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
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/**
 * Read up to `cap` bytes of `path` into `buf`, returning the byte count, or -1
 * with `errno` set by `open` when the file cannot be opened. A read error ends
 * the file early.
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
 * Read `path` if it is there, else `alt`, into `buf`. Returns the length, or -1
 * when the file chosen cannot be opened.
 *
 * Only absence (`ENOENT` or `ENOTDIR`) falls through to `alt`. A file that is
 * there and cannot be opened may have been made so on purpose, and the copy
 * at the root would then be configuration somebody took away.
 */
static int read_either(const char *path, const char *alt, char *buf, int cap)
{
    int n = read_file(path, buf, cap);

    if (n < 0 && (errno == ENOENT || errno == ENOTDIR)) {
        n = read_file(alt, buf, cap);
    }

    return n;
}

/** Busy-wait for `ms` milliseconds, measured by gettimeofday. */
static void delay_ms(int ms)
{
    struct timeval start, now;
    long elapsed;

    gettimeofday(&start, NULL);
    do {
        gettimeofday(&now, NULL);
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
    } while (elapsed < ms);
}

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

/** A word longer than 63 bytes is compared by its first 63. */
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

int dns_build_query(unsigned char *buf, int cap, const char *name, unsigned short id)
{
    int pos;
    const char *p = name;

    if (cap < 12 || *name == '\0') {
        return -1;
    }

    /* 12-byte header: id, flags RD=1, one question, no answers. */
    buf[0] = (u8)(id >> 8);
    buf[1] = (u8)id;
    buf[2] = 0x01;
    buf[3] = 0x00; /* QR=0, RD=1 */
    buf[4] = 0x00;
    buf[5] = 0x01; /* QDCOUNT = 1 */
    buf[6] = 0x00;
    buf[7] = 0x00; /* ANCOUNT */
    buf[8] = 0x00;
    buf[9] = 0x00; /* NSCOUNT */
    buf[10] = 0x00;
    buf[11] = 0x00; /* ARCOUNT */
    pos = 12;

    /* QNAME: one length-prefixed label per dot-separated component. */
    while (*p) {
        const char *start = p;
        int label = 0, k;

        while (*p && *p != '.') {
            p++;
            label++;
        }
        if (label == 0 || label > 63) {
            return -1;
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

    buf[pos++] = 0x00; /* root label terminates QNAME */
    buf[pos++] = 0x00;
    buf[pos++] = 0x01; /* QTYPE  = A  */
    buf[pos++] = 0x00;
    buf[pos++] = 0x01; /* QCLASS = IN */
    return pos;
}

/**
 * The offset just past the domain name at offset `pos` of `resp`, or -1 for a
 * name that runs past `len` or has a label of a reserved type.
 *
 * A name is length-prefixed labels ended by a zero byte or by a two-byte
 * compression pointer, whose top two bits are set. Pointers are not
 * followed, since skipping is enough to reach the fixed fields, so this
 * cannot loop.
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
            return -1; /* reserved label type */
        }
        pos += 1 + b; /* length-prefixed label */
    }
    return -1;
}

int dns_parse_a(const unsigned char *resp, int len, unsigned short id, unsigned int *out_ip)
{
    int pos, i, qd, an;

    if (len < 12) {
        return -1;
    }
    if (((resp[0] << 8) | resp[1]) != (int)id) {
        return -1;
    }
    if ((resp[2] & 0x80) == 0) {
        return -1; /* not a response */
    }
    if ((resp[3] & 0x0F) != 0) {
        return -1; /* RCODE != 0 (e.g. NXDOMAIN) */
    }
    qd = (resp[4] << 8) | resp[5];
    an = (resp[6] << 8) | resp[7];
    if (an < 1) {
        return -1;
    }

    pos = 12;
    for (i = 0; i < qd; i++) {
        pos = dns_skip_name(resp, len, pos);
        if (pos < 0 || pos + 4 > len) {
            return -1;
        }
        pos += 4; /* QTYPE + QCLASS */
    }

    for (i = 0; i < an; i++) {
        int type, cls, rdlen;

        pos = dns_skip_name(resp, len, pos);
        if (pos < 0 || pos + 10 > len) {
            return -1;
        }
        type = (resp[pos + 0] << 8) | resp[pos + 1];
        cls = (resp[pos + 2] << 8) | resp[pos + 3];
        rdlen = (resp[pos + 8] << 8) | resp[pos + 9];
        pos += 10; /* TYPE CLASS TTL RDLENGTH */
        if (pos + rdlen > len) {
            return -1;
        }
        if (type == 1 && cls == 1 && rdlen == 4) {
            *out_ip = ((u32)resp[pos + 0] << 24) | ((u32)resp[pos + 1] << 16) |
                      ((u32)resp[pos + 2] << 8) | (u32)resp[pos + 3];
            return 0;
        }
        pos += rdlen;
    }
    return -1;
}

/**
 * The address of the first `nameserver` line with a valid address in the
 * first 256 bytes of the resolver configuration, or `DNS_DEFAULT_NS` when
 * there is none or the file cannot be read.
 */
static u32 read_nameserver(void)
{
    char buf[256];
    int n, i;

    n = read_either(RESOLV_CONF, RESOLV_CONF_ALT, buf, (int)sizeof(buf));
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
            if (t < 15)
                key[t++] = buf[j];
            j++;
        }
        key[t] = '\0';
        if (str_eq(key, "nameserver")) {
            while (j < le && (buf[j] == ' ' || buf[j] == '\t')) {
                j++;
            }
            t = 0;
            while (j < le && buf[j] != ' ' && buf[j] != '\t') {
                if (t < 63)
                    val[t++] = buf[j];
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
 * Resolve `name` by an A query to the nameserver `ns` on UDP port 53, with
 * the address in `*out_ip`; 0, or -1.
 *
 * The query is sent up to 30 times, each followed by four reads with
 * `MSG_DONTWAIT` 50 ms apart, about six seconds in all, since UDP resends
 * nothing and a query or its reply can be lost. The socket is connected, so
 * only datagrams from that address and port arrive. The first response with
 * the query's id ends the wait, whatever it says (an address, NXDOMAIN, or no
 * A record); anything else is dropped. The id is taken from the clock.
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
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    for (attempt = 0; attempt < 30 && !got; attempt++) {
        sendto(fd, query, (size_t)qlen, 0, (struct sockaddr *)&sa, sizeof(sa));

        for (poll = 0; poll < 4 && !got; poll++) {
            delay_ms(50);
            n = (int)recvfrom(fd, resp, sizeof(resp), MSG_DONTWAIT, NULL, NULL);
            if (n >= 12 && ((resp[0] << 8) | resp[1]) == (int)id && (resp[2] & 0x80)) {
                rc = dns_parse_a(resp, n, id, out_ip);
                got = 1;
            }
        }
    }

    close(fd);
    return got ? rc : -1;
}

int resolve_host(const char *host, unsigned int *out_ip)
{
    char fbuf[1024];
    int n;

    if (rach2_inet_aton(host, out_ip) == 0) {
        return 0;
    }

    n = read_either(HOSTS_PATH, HOSTS_PATH_ALT, fbuf, (int)sizeof(fbuf));
    if (n > 0 && hosts_find(fbuf, n, host, out_ip) == 0) {
        return 0;
    }

    return dns_query(host, read_nameserver(), out_ip);
}
