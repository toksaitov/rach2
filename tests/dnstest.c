/**
 * Exercises the userspace stub resolver in utils/resolv.c. The helpers that
 * use no network run without one: inet_aton on good and bad literals,
 * hosts_find against a crafted hosts buffer, resolve_host on literals and on
 * names read from the image's hosts file (/etc/hosts, or else /hosts, which
 * the test image stages at its root), and dns_parse_a and dns_build_query
 * against crafted wire data. With `--live`, a last block resolves names
 * through SLIRP's forwarder and needs the host's internet.
 *
 * Linked against the ported musl, the same libc utils/resolv.c uses.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row or group of rows that `check` names, failed with
 * its first failed row: `aton-valid`, `aton-invalid`, `hosts-name`,
 * `hosts-alias`, `hosts-case`, `hosts-localhost`, `hosts-alias-last`,
 * `hosts-miss`, `hosts-comment`, `resolve-literal`, `resolve-localhost`,
 * `resolve-hosts-file`, `parse-a`, `parse-cname`, `parse-id-mismatch`,
 * `parse-nxdomain`, `parse-nodata`, `parse-truncated`, `parse-bad-name`,
 * `parse-short`, `query-length`, `query-id`, `query-rd`, `query-qdcount`,
 * `query-labels`, `query-qtype`, `query-qclass` and `query-refusals`, and
 * with `--live` alone `live-resolve` and `live-nxdomain`.
 *
 * Needs: the test image's hosts file, whose addresses of QEMU's user network
 * the rows for gateway, dns and rach2 expect. On a machine with its own hosts
 * file those three rows read that file and fail on its contents, not on the
 * resolver.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../utils/netdb.h"
#include "probe_report.h"

static int passed = 0, failed = 0;

/** Print `desc` with OK or FAILED, count the result, and record it in the
 * part of the assertion `id` (probe_report.h), which a run of rows of one
 * assertion shares. */
static void check(const char *id, const char *desc, int ok)
{
    static const char *open;

    if (open == NULL || strcmp(open, id) != 0) {
        report_part(id);
        open = id;
    }
    printf("  %s: %s\n", desc, ok ? "OK" : "FAILED");
    if (ok) {
        passed++;
    } else {
        failed++;
        report_miss(desc);
    }
}

/** Print `label` and the address `ip`, in host order, as a dotted quad. */
static void print_ip(const char *label, uint32_t ip)
{
    printf("    %s -> %u.%u.%u.%u\n", label, (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
           ip & 0xFF);
}

/*
 * Crafted DNS responses (RFC 1035) for hermetic parser tests. The question in
 * each is "example.com"; answers reference it with a 0xC00C compression
 * pointer.
 */

/* Normal: one A record, 93.184.216.34. id 0x1234. */
static const unsigned char v_a[] = {
    0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 'e',  'x',
    'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01, 0xC0,
    0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x04, 0x5D, 0xB8, 0xD8, 0x22};

/* CNAME record followed by the A record, 8.8.4.4. id 0xABCD. */
static const unsigned char v_cname[] = {
    0xAB, 0xCD, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x07, 'e',  'x',  'a',
    'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01, 0xC0, 0x0C, 0x00,
    0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x07, 0x01, 'a',  0x03, 'c',  'o',  'm',  0x00,
    0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x04, 0x08, 0x08, 0x04, 0x04};

/* NXDOMAIN: RCODE 3. id 0x0001. */
static const unsigned char v_nx[] = {0x00, 0x01, 0x81, 0x83, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',
                                     0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01};

/* NODATA: RCODE 0 but ANCOUNT 0. id 0x0002. */
static const unsigned char v_nodata[] = {0x00, 0x02, 0x81, 0x80, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',
                                         0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01};

/* Truncated: claims RDLENGTH 4 but only 2 RDATA bytes follow. id 0x0003. */
static const unsigned char v_trunc[] = {
    0x00, 0x03, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 'e',  'x',
    'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01, 0xC0,
    0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x04, 0x08, 0x08};

/* Malformed: answer name uses a reserved label type (0x40). id 0x0004. */
static const unsigned char v_badname[] = {
    0x00, 0x04, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 'e',  'x',  'a',
    'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01, 0x40, 0x00, 0x01};

/** Resolve real names through SLIRP's forwarder, which needs the host's
 * internet. */
static void live_rows(void)
{
    uint32_t ip = 0;
    int r;

    puts("live DNS (via SLIRP 10.0.2.3, needs host internet):");
    r = resolve_host("one.one.one.one", &ip);
    check("live-resolve", "one.one.one.one resolves", r == 0);
    if (r == 0) {
        print_ip("one.one.one.one", ip);
    }
    r = resolve_host("dns.google", &ip);
    check("live-resolve", "dns.google resolves", r == 0);
    if (r == 0) {
        print_ip("dns.google", ip);
    }
    check("live-nxdomain", "nxdomain.invalid fails cleanly",
          resolve_host("nxdomain.invalid", &ip) != 0);
}

/** Run every row, and the live rows with `--live`; 0 when every row
 * passed, 1 otherwise. */
int main(int argc, char **argv, char **envp)
{
    int live = (argc > 1 && strcmp(argv[1], "--live") == 0);
    uint32_t ip = 0;
    unsigned char qb[64];
    int ql;
    const char *hosts = "10.0.2.2 gw gateway\n"
                        "127.0.0.1 localhost\n"
                        "# 8.8.4.4 commented\n"
                        "8.8.8.8 dns google\n";
    int hlen = (int)strlen(hosts);

    report_begin(report_case(envp, "guest.default.dnstest"));
    puts("DNS resolver test starting...");
    puts("");

    puts("inet_aton (numeric literals):");
    check("aton-valid", "10.0.2.2", rach2_inet_aton("10.0.2.2", &ip) == 0 && ip == 0x0A000202U);
    check("aton-valid", "127.0.0.1", rach2_inet_aton("127.0.0.1", &ip) == 0 && ip == 0x7F000001U);
    check("aton-valid", "0.0.0.0", rach2_inet_aton("0.0.0.0", &ip) == 0 && ip == 0x00000000U);
    check("aton-valid", "255.255.255.255",
          rach2_inet_aton("255.255.255.255", &ip) == 0 && ip == 0xFFFFFFFFU);
    check("aton-invalid", "reject 999.1.1.1", rach2_inet_aton("999.1.1.1", &ip) != 0);
    check("aton-invalid", "reject 1.2.3", rach2_inet_aton("1.2.3", &ip) != 0);
    check("aton-invalid", "reject 1.2.3.4.5", rach2_inet_aton("1.2.3.4.5", &ip) != 0);
    check("aton-invalid", "reject 1.2.3.", rach2_inet_aton("1.2.3.", &ip) != 0);
    check("aton-invalid", "reject abc", rach2_inet_aton("abc", &ip) != 0);
    check("aton-invalid", "reject empty", rach2_inet_aton("", &ip) != 0);

    puts("hosts_find (crafted buffer):");
    check("hosts-name", "gateway",
          hosts_find(hosts, hlen, "gateway", &ip) == 0 && ip == 0x0A000202U);
    check("hosts-alias", "gw alias", hosts_find(hosts, hlen, "gw", &ip) == 0 && ip == 0x0A000202U);
    check("hosts-case", "GATEWAY ci",
          hosts_find(hosts, hlen, "GATEWAY", &ip) == 0 && ip == 0x0A000202U);
    check("hosts-localhost", "localhost",
          hosts_find(hosts, hlen, "localhost", &ip) == 0 && ip == 0x7F000001U);
    check("hosts-alias-last", "google",
          hosts_find(hosts, hlen, "google", &ip) == 0 && ip == 0x08080808U);
    check("hosts-miss", "miss zzz", hosts_find(hosts, hlen, "zzz", &ip) != 0);
    check("hosts-comment", "comment not matched", hosts_find(hosts, hlen, "commented", &ip) != 0);

    puts("resolve_host (literal + real /hosts):");
    check("resolve-literal", "literal 10.0.2.2",
          resolve_host("10.0.2.2", &ip) == 0 && ip == 0x0A000202U);
    check("resolve-localhost", "localhost",
          resolve_host("localhost", &ip) == 0 && ip == 0x7F000001U);
    check("resolve-hosts-file", "gateway", resolve_host("gateway", &ip) == 0 && ip == 0x0A000202U);
    check("resolve-hosts-file", "dns", resolve_host("dns", &ip) == 0 && ip == 0x0A000203U);
    check("resolve-hosts-file", "rach2", resolve_host("rach2", &ip) == 0 && ip == 0x0A00020FU);

    puts("dns_parse_a (crafted responses):");
    check("parse-a", "A record",
          dns_parse_a(v_a, (int)sizeof(v_a), 0x1234, &ip) == 0 && ip == 0x5DB8D822U);
    check("parse-cname", "CNAME then A",
          dns_parse_a(v_cname, (int)sizeof(v_cname), 0xABCD, &ip) == 0 && ip == 0x08080404U);
    check("parse-id-mismatch", "id mismatch", dns_parse_a(v_a, (int)sizeof(v_a), 0x9999, &ip) != 0);
    check("parse-nxdomain", "NXDOMAIN", dns_parse_a(v_nx, (int)sizeof(v_nx), 0x0001, &ip) != 0);
    check("parse-nodata", "NODATA", dns_parse_a(v_nodata, (int)sizeof(v_nodata), 0x0002, &ip) != 0);
    check("parse-truncated", "truncated rdata",
          dns_parse_a(v_trunc, (int)sizeof(v_trunc), 0x0003, &ip) != 0);
    check("parse-bad-name", "malformed name",
          dns_parse_a(v_badname, (int)sizeof(v_badname), 0x0004, &ip) != 0);
    check("parse-short", "short buffer", dns_parse_a(v_a, 8, 0x1234, &ip) != 0);

    puts("dns_build_query (encoding):");
    ql = dns_build_query(qb, (int)sizeof(qb), "example.com", 0x1234);
    check("query-length", "length 29", ql == 29);
    check("query-id", "id high", qb[0] == 0x12 && qb[1] == 0x34);
    check("query-rd", "RD flag", qb[2] == 0x01 && qb[3] == 0x00);
    check("query-qdcount", "qdcount 1", qb[4] == 0x00 && qb[5] == 0x01);
    check("query-labels", "label sizes", qb[12] == 0x07 && qb[20] == 0x03 && qb[24] == 0x00);
    check("query-labels", "first label", qb[13] == 'e' && qb[14] == 'x');
    check("query-qtype", "qtype A", qb[25] == 0x00 && qb[26] == 0x01);
    check("query-qclass", "qclass IN", qb[27] == 0x00 && qb[28] == 0x01);
    check("query-refusals", "reject empty", dns_build_query(qb, (int)sizeof(qb), "", 0x1) < 0);
    check("query-refusals", "reject dotdot", dns_build_query(qb, (int)sizeof(qb), "a..b", 0x1) < 0);
    check("query-refusals", "reject leadingdot",
          dns_build_query(qb, (int)sizeof(qb), ".com", 0x1) < 0);
    check("query-refusals", "reject small cap", dns_build_query(qb, 5, "example.com", 0x1) < 0);

    if (live) {
        live_rows();
    } else {
        puts("live DNS: skipped; pass --live to resolve real names");
    }

    report_finish();
    puts("");
    printf("Results: %d passed, %d failed\n", passed, failed);

    if (failed == 0) {
        puts("All DNS tests passed!");
        return 0;
    }
    puts("Some tests failed!");
    return 1;
}
