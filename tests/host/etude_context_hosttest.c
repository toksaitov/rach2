/* Check context input hashes, deployment paths and preparation identity.
 * host-deps: tools/etude/measurement_context.c tools/etude/json.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-cflags: -I tools/etude -I tools/etude/include -I utils
 * host-cflags: -Wall -Wextra -Werror
 */
#include "../../tools/etude/measurement_context.h"
#include "../../utils/sha256.h"
#include "compat/fixdir_host.c"
#include <stdint.h>
#include <signal.h>

static int failures;
static const char commit[] = "1111111111111111111111111111111111111111";
static struct etude_preparation_receipt receipt;
static int with_observer = 1;
#define CHECK(held, message) do { if (!(held)) { \
    fprintf(stderr, "etude-context: %s\n", message); failures++; } } while (0)

static void stopped(int sig)
{
    exit(128 + sig);
}
static void put(const char *name, const char *text)
{
    FILE *f = fopen(fix_path(name), "wb");
    if (!f || fputs(text, f) < 0 || fclose(f))
        exit(2);
}
/* The fixture inputs are small; hash the exact on-disk bytes. */
static void descriptor(const char *name, char out[2048], char hash[65])
{
    struct sha256 s;
    unsigned char bytes[65536], digest[32];
    FILE *f = fopen(fix_path(name), "rb");
    if (!f)
        exit(2);
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    if (ferror(f) || !feof(f) || fclose(f))
        exit(2);
    sha256_init(&s);
    sha256_update(&s, bytes, n);
    sha256_final(&s, digest);
    sha256_hex(digest, hash);
    snprintf(out, 2048, "{\"path\":\"%s\",\"sha256\":\"%s\",\"size_bytes\":%lu}",
             fix_path(name), hash, (unsigned long)n);
}
static void snapshots(int bad_source)
{
    char data[8192], source[2048], tool[2048], policy[2048], hash[65];
    descriptor("input", source, hash);
    char *at = strstr(source, "\"sha256\"");
    if (!at)
        exit(2);
    snprintf(data, sizeof(data),
             "{\"schema_version\":1,\"commit\":\"%s\",\"inputs\":[{\"path\":\"%s\",%s]}",
             commit, bad_source ? "../input" : "input", at);
    put("source", data);
    descriptor("tool", tool, hash);
    snprintf(data, sizeof(data), "{\"schema_version\":1,\"inputs\":[%s]}", tool);
    put("tools", data);
    descriptor("policy", policy, hash);
    snprintf(data, sizeof(data), "{\"schema_version\":1,\"policy\":%s}", policy);
    put("site", data);
    put("cache", "{\"schema_version\":1,\"controlled\":true,"
                 "\"method\":\"verified artifacts; OS cache natural after preparation\"}");
}
static void context(const char *receipt_name)
{
    char s[2048], t[2048], p[2048], cache[2048], artifacts[2048], prep[2048], hash[65];
    char observer[4096];
    char text[16384];
    descriptor("source", s, hash);
    descriptor("tools", t, hash);
    descriptor("site", p, hash);
    descriptor("cache", cache, hash);
    descriptor("record", artifacts, hash);
    if (receipt_name)
        descriptor(receipt_name, prep, hash);
    else
        strcpy(prep, "null");
    if (with_observer) {
        descriptor("trace", observer, hash);
        snprintf(observer, sizeof(observer),
                 "{\"trace\":\"%s\",\"stop_request\":\"%s\",\"header_sha256\":\"%s\"}",
                 fix_path("trace"), fix_path("stop"), hash);
    } else
        strcpy(observer, "null");
    snprintf(text, sizeof(text),
             "{\"format\":\"etude-measurement-context\",\"schema_version\":1,"
             "\"run_id\":\"context-test\",\"commit\":\"%s\",\"purpose\":\"focused\","
             "\"source\":%s,\"artifacts\":%s,\"site_profile\":%s,\"tools\":%s,"
             "\"cache_state\":%s,\"preparation\":%s,\"exemption\":null,\"outer_observation\":%s}",
             commit, s, artifacts, p, t, cache, prep, observer);
    put("context", text);
}
static int open_context(struct etude_measurement_context **c)
{
    char error[256];
    const struct etude_context_inputs inputs = {
        fix_dir(), fix_path("record"), fix_path("policy"), commit, fix_path("tool")
    };
    return etude_context_open(fix_path("context"), &inputs, c, error, sizeof(error));
}
static void bad_receipt(const char *name, int kind)
{
    struct etude_preparation_receipt r = receipt;
    if (kind == 0)
        strcpy(r.run_id, "another-run");
    else if (kind == 1)
        strcpy(r.clock.boot_id, "another-boot");
    else
        r.end.monotonic_us += UINT64_C(1000000000);
    CHECK(!etude_context_receipt_write(fix_path(name), &r), "negative receipt writes");
    context(name);
    struct etude_measurement_context *c = NULL;
    CHECK(open_context(&c) != 0 && !c, "unrelated or future preparation is refused");
    etude_context_close(c);
}
static void test_grants(void)
{
    struct etude_preparation_grant grant = {receipt.clock, receipt.start}, loaded;
    const char *claim = "context-test:prepare";
    CHECK(!etude_context_grant_write(fix_path("grant"), claim, &grant), "grant writes once");
    CHECK(!etude_context_grant_read(fix_path("grant"), claim, &loaded) &&
          loaded.start.monotonic_us == grant.start.monotonic_us &&
          !strcmp(loaded.clock.boot_id, grant.clock.boot_id), "first grant reading is preserved");
    CHECK(etude_context_grant_write(fix_path("grant"), claim, &grant) != 0,
          "an existing grant is never replaced");
    CHECK(etude_context_grant_read(fix_path("grant"), "unrelated:prepare", &loaded) != 0,
          "another claim cannot supply the start");
    CHECK(etude_context_grant_write("relative-grant", claim, &grant) != 0,
          "grant requires an absolute output path");
    strcpy(grant.clock.boot_id, "another-boot");
    CHECK(!etude_context_grant_write(fix_path("grant-boot"), claim, &grant) &&
          etude_context_grant_read(fix_path("grant-boot"), claim, &loaded) != 0,
          "a grant from another boot is refused");
    grant.clock = receipt.clock;
    grant.start.monotonic_us += UINT64_C(10000000000);
    CHECK(!etude_context_grant_write(fix_path("grant-future"), claim, &grant) &&
          etude_context_grant_read(fix_path("grant-future"), claim, &loaded) != 0,
          "a future grant is refused");
    put("grant-malformed", "{}\n");
    CHECK(etude_context_grant_read(fix_path("grant-malformed"), claim, &loaded) != 0,
          "a malformed grant is refused");
}

int main(void)
{
    char descriptor_text[2048], error[256];
    struct etude_measurement_context *c = NULL;
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
#ifndef _WIN32
    signal(SIGHUP, stopped);
#endif
    fix_dir();
    put("input", "committed source\n");
    put("tool", "native executable fixture\n");
    put("record", "producer record fixture\n");
    put("policy", "installed policy fixture\n");
    snapshots(0);
    strcpy(receipt.run_id, "context-test");
    strcpy(receipt.commit, commit);
    descriptor("record", descriptor_text, receipt.record_sha256);
    CHECK(!etude_context_clock(&receipt.clock), "native clock identity exists");
    CHECK(!etude_history_now(&receipt.start), "preparation start is read");
    receipt.end = receipt.start;
    test_grants();
#ifdef _WIN32
    const char *platform = "windows-native-counters-v1";
#elif defined(__APPLE__)
    const char *platform = "macos-mach-libproc-v1";
#else
    const char *platform = "linux-proc-counters-v1";
#endif
    char header[2048];
    snprintf(header, sizeof(header),
             "{\"kind\":\"start\",\"schema_version\":1,\"method\":\"native-load-v1\","
             "\"platform\":\"%s\",\"machine_id\":\"%s\",\"boot_id\":\"%s\","
             "\"run_id\":\"context-test\",\"root_pid\":1,\"root_creation\":\"1\","
             "\"observer_pid\":2,\"observer_creation\":\"2\","
             "\"requested_interval_us\":1000000,\"process_limit\":4096}\n",
             platform, receipt.clock.machine_id, receipt.clock.boot_id);
    put("trace", header);
    receipt.passed = receipt.cleanup_confirmed = 1;
    CHECK(!etude_context_receipt_write(fix_path("receipt"), &receipt), "successful receipt writes");
    CHECK(etude_context_receipt_write(fix_path("receipt"), &receipt) != 0,
          "an existing receipt is never replaced");
    receipt.cleanup_confirmed = 0;
    CHECK(etude_context_receipt_write(fix_path("unconfirmed"), &receipt) != 0,
          "unconfirmed cleanup cannot produce a receipt");
    receipt.cleanup_confirmed = 1;
    context("receipt");
    CHECK(!open_context(&c) && c, "matching deployed source and native tools validate");
    if (c) {
        const struct etude_preparation_receipt *r = etude_context_preparation(c);
        CHECK(r && r->same_run && !r->elapsed.reason, "same-run duration is exact");
        CHECK(etude_context_eligible(c), "controlled method and outer header validate");
        FILE *trace = fopen(fix_path("trace"), "ab");
        if (!trace || fputs("{\"sample\":1}\n", trace) < 0 || fclose(trace))
            exit(2);
        CHECK(!etude_context_recheck(c, error, sizeof(error)), "live trace growth is allowed");
        put("trace", "{}\n");
        CHECK(etude_context_recheck(c, error, sizeof(error)) != 0, "changed header is refused");
        put("trace", header);
        put("tool", "changed native tool\n");
        CHECK(etude_context_recheck(c, error, sizeof(error)) != 0, "changed tool is refused");
        put("tool", "native executable fixture\n");
        CHECK(!etude_context_recheck(c, error, sizeof(error)), "unchanged inputs revalidate");
        put("context", "{}");
        CHECK(etude_context_recheck(c, error, sizeof(error)) != 0, "context mutation is refused");
    }
    etude_context_close(c);
    bad_receipt("other-run", 0);
    bad_receipt("other-boot", 1);
    bad_receipt("future", 2);
    with_observer = 0;
    snapshots(0);
    context(NULL);
    c = NULL;
    CHECK(!open_context(&c) && !etude_context_eligible(c),
          "missing outer observation is correctness only");
    etude_context_close(c);
    snapshots(1);
    context(NULL);
    c = NULL;
    CHECK(open_context(&c) != 0 && !c, "source paths cannot escape the deployment tree");
    etude_context_close(c);
    if (!failures)
        fix_passed();
    return failures ? 1 : 0;
}
