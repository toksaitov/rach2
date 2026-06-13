/*
 * dnstest.c - name resolver test program for Rach2 OS
 *
 * Exercises the userspace stub resolver in utils/resolv.c. The pure helpers are
 * tested hermetically (no network): inet_aton on good and bad literals,
 * hosts_find against a crafted /hosts buffer, and resolve_host on literals and
 * names read from the real /hosts staged in the image root. The DNS wire parser
 * (dns_parse_a) and live resolution are added in later phases.
 *
 * Compile with the shared resolver:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 \
 *       -o dnstest dnstest.c ../utils/resolv.c
 */

#include "../utils/netdb.h"

typedef unsigned int   size_t;
typedef unsigned int   uint32_t;

#define STDOUT_FILENO 1

#define SYS_EXIT  1
#define SYS_WRITE 4

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s) { sys_write(STDOUT_FILENO, s, strlen(s)); }
static void puts(const char *s)  { print(s); sys_write(STDOUT_FILENO, "\n", 1); }

static void print_num(int n);

static void print_ip(uint32_t ip)
{
    print_num((int)((ip >> 24) & 0xFF)); print(".");
    print_num((int)((ip >> 16) & 0xFF)); print(".");
    print_num((int)((ip >>  8) & 0xFF)); print(".");
    print_num((int)( ip        & 0xFF));
}

static void print_num(int n)
{
    char buf[12];
    int i = 0;
    if (n < 0) { sys_write(STDOUT_FILENO, "-", 1); n = -n; }
    if (n == 0) buf[i++] = '0';
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) sys_write(STDOUT_FILENO, &buf[--i], 1);
}

static int passed = 0, failed = 0;

static void check(const char *desc, int ok)
{
    print("  "); print(desc); print(": ");
    if (ok) { puts("OK"); passed++; }
    else    { puts("FAILED"); failed++; }
}

/*
 * Crafted DNS responses (RFC 1035) for hermetic parser tests. The question in
 * each is "example.com"; answers reference it with a 0xC00C compression pointer.
 */

/* Normal: one A record, 93.184.216.34. id 0x1234. */
static const unsigned char v_a[] = {
    0x12,0x34, 0x81,0x80, 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x00,
    0x07,'e','x','a','m','p','l','e', 0x03,'c','o','m', 0x00, 0x00,0x01, 0x00,0x01,
    0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x01,0x00, 0x00,0x04, 0x5D,0xB8,0xD8,0x22
};

/* CNAME record followed by the A record, 8.8.4.4. id 0xABCD. */
static const unsigned char v_cname[] = {
    0xAB,0xCD, 0x81,0x80, 0x00,0x01, 0x00,0x02, 0x00,0x00, 0x00,0x00,
    0x07,'e','x','a','m','p','l','e', 0x03,'c','o','m', 0x00, 0x00,0x01, 0x00,0x01,
    0xC0,0x0C, 0x00,0x05, 0x00,0x01, 0x00,0x00,0x01,0x00, 0x00,0x07,
        0x01,'a', 0x03,'c','o','m', 0x00,
    0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x01,0x00, 0x00,0x04, 0x08,0x08,0x04,0x04
};

/* NXDOMAIN: RCODE 3. id 0x0001. */
static const unsigned char v_nx[] = {
    0x00,0x01, 0x81,0x83, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
    0x07,'e','x','a','m','p','l','e', 0x03,'c','o','m', 0x00, 0x00,0x01, 0x00,0x01
};

/* NODATA: RCODE 0 but ANCOUNT 0. id 0x0002. */
static const unsigned char v_nodata[] = {
    0x00,0x02, 0x81,0x80, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
    0x07,'e','x','a','m','p','l','e', 0x03,'c','o','m', 0x00, 0x00,0x01, 0x00,0x01
};

/* Truncated: claims RDLENGTH 4 but only 2 RDATA bytes follow. id 0x0003. */
static const unsigned char v_trunc[] = {
    0x00,0x03, 0x81,0x80, 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x00,
    0x07,'e','x','a','m','p','l','e', 0x03,'c','o','m', 0x00, 0x00,0x01, 0x00,0x01,
    0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x01,0x00, 0x00,0x04, 0x08,0x08
};

/* Malformed: answer name uses a reserved label type (0x40). id 0x0004. */
static const unsigned char v_badname[] = {
    0x00,0x04, 0x81,0x80, 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x00,
    0x07,'e','x','a','m','p','l','e', 0x03,'c','o','m', 0x00, 0x00,0x01, 0x00,0x01,
    0x40, 0x00,0x01
};

void _start(void)
{
    uint32_t ip = 0;
    unsigned char qb[64];
    int ql, r;
    const char *hosts =
        "10.0.2.2 gw gateway\n"
        "127.0.0.1 localhost\n"
        "# 8.8.4.4 commented\n"
        "8.8.8.8 dns google\n";
    int hlen = (int)strlen(hosts);

    puts("DNS resolver test starting...");
    puts("");

    puts("inet_aton (numeric literals):");
    check("10.0.2.2",        inet_aton("10.0.2.2", &ip) == 0 && ip == 0x0A000202U);
    check("127.0.0.1",       inet_aton("127.0.0.1", &ip) == 0 && ip == 0x7F000001U);
    check("0.0.0.0",         inet_aton("0.0.0.0", &ip) == 0 && ip == 0x00000000U);
    check("255.255.255.255", inet_aton("255.255.255.255", &ip) == 0 && ip == 0xFFFFFFFFU);
    check("reject 999.1.1.1", inet_aton("999.1.1.1", &ip) != 0);
    check("reject 1.2.3",     inet_aton("1.2.3", &ip) != 0);
    check("reject 1.2.3.4.5", inet_aton("1.2.3.4.5", &ip) != 0);
    check("reject 1.2.3.",    inet_aton("1.2.3.", &ip) != 0);
    check("reject abc",       inet_aton("abc", &ip) != 0);
    check("reject empty",     inet_aton("", &ip) != 0);

    puts("hosts_find (crafted buffer):");
    check("gateway",   hosts_find(hosts, hlen, "gateway", &ip) == 0 && ip == 0x0A000202U);
    check("gw alias",  hosts_find(hosts, hlen, "gw", &ip) == 0 && ip == 0x0A000202U);
    check("GATEWAY ci", hosts_find(hosts, hlen, "GATEWAY", &ip) == 0 && ip == 0x0A000202U);
    check("localhost", hosts_find(hosts, hlen, "localhost", &ip) == 0 && ip == 0x7F000001U);
    check("google",    hosts_find(hosts, hlen, "google", &ip) == 0 && ip == 0x08080808U);
    check("miss zzz",  hosts_find(hosts, hlen, "zzz", &ip) != 0);
    check("comment not matched", hosts_find(hosts, hlen, "commented", &ip) != 0);

    puts("resolve_host (literal + real /hosts):");
    check("literal 10.0.2.2", resolve_host("10.0.2.2", &ip) == 0 && ip == 0x0A000202U);
    check("localhost", resolve_host("localhost", &ip) == 0 && ip == 0x7F000001U);
    check("gateway",   resolve_host("gateway", &ip) == 0 && ip == 0x0A000202U);
    check("dns",       resolve_host("dns", &ip) == 0 && ip == 0x0A000203U);
    check("rach2",     resolve_host("rach2", &ip) == 0 && ip == 0x0A00020FU);

    puts("dns_parse_a (crafted responses):");
    check("A record",        dns_parse_a(v_a, (int)sizeof(v_a), 0x1234, &ip) == 0 && ip == 0x5DB8D822U);
    check("CNAME then A",    dns_parse_a(v_cname, (int)sizeof(v_cname), 0xABCD, &ip) == 0 && ip == 0x08080404U);
    check("id mismatch",     dns_parse_a(v_a, (int)sizeof(v_a), 0x9999, &ip) != 0);
    check("NXDOMAIN",        dns_parse_a(v_nx, (int)sizeof(v_nx), 0x0001, &ip) != 0);
    check("NODATA",          dns_parse_a(v_nodata, (int)sizeof(v_nodata), 0x0002, &ip) != 0);
    check("truncated rdata", dns_parse_a(v_trunc, (int)sizeof(v_trunc), 0x0003, &ip) != 0);
    check("malformed name",  dns_parse_a(v_badname, (int)sizeof(v_badname), 0x0004, &ip) != 0);
    check("short buffer",    dns_parse_a(v_a, 8, 0x1234, &ip) != 0);

    puts("dns_build_query (encoding):");
    ql = dns_build_query(qb, (int)sizeof(qb), "example.com", 0x1234);
    check("length 29",   ql == 29);
    check("id high",     qb[0] == 0x12 && qb[1] == 0x34);
    check("RD flag",     qb[2] == 0x01 && qb[3] == 0x00);
    check("qdcount 1",   qb[4] == 0x00 && qb[5] == 0x01);
    check("label sizes", qb[12] == 0x07 && qb[20] == 0x03 && qb[24] == 0x00);
    check("first label", qb[13] == 'e' && qb[14] == 'x');
    check("qtype A",     qb[25] == 0x00 && qb[26] == 0x01);
    check("qclass IN",   qb[27] == 0x00 && qb[28] == 0x01);
    check("reject empty",      dns_build_query(qb, (int)sizeof(qb), "", 0x1) < 0);
    check("reject dotdot",     dns_build_query(qb, (int)sizeof(qb), "a..b", 0x1) < 0);
    check("reject leadingdot", dns_build_query(qb, (int)sizeof(qb), ".com", 0x1) < 0);
    check("reject small cap",  dns_build_query(qb, 5, "example.com", 0x1) < 0);

    puts("live DNS (via SLIRP 10.0.2.3, needs host internet):");
    r = resolve_host("one.one.one.one", &ip);
    check("one.one.one.one resolves", r == 0);
    if (r == 0) { print("    one.one.one.one -> "); print_ip(ip); puts(""); }
    r = resolve_host("dns.google", &ip);
    check("dns.google resolves", r == 0);
    if (r == 0) { print("    dns.google -> "); print_ip(ip); puts(""); }
    check("nxdomain.invalid fails cleanly", resolve_host("nxdomain.invalid", &ip) != 0);

    puts("");
    print("Results: "); print_num(passed); print(" passed, ");
    print_num(failed); puts(" failed");

    if (failed == 0) {
        puts("All DNS tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
