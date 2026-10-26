/* Native load observations retain sampling limits and creation identities.
 * host-deps: tools/etude/authority.c tools/etude/authority_parse.c
 * host-deps: tools/etude/posix/authority_platform.c
 * host-deps: tools/etude/win/authority_platform.c
 * host-deps: tools/etude/routine_proof.c tools/etude/routine_owner.c
 * host-deps: tools/etude/routine_direct.c
 * host-deps: tools/etude/routine_charge.c
 * host-deps: tools/etude/observe.c tools/etude/observe_import.c
 * host-deps: tools/etude/observe_scope.c tools/etude/observe_system.c
 * host-deps: tools/etude/observe_sequence.c
 * host-deps: tools/etude/posix/observe_sequence.c
 * host-deps: tools/etude/boot_identity.c
 * host-deps: tools/etude/measurement_context.c tools/etude/history.c
 * host-deps: tools/etude/history_json.c tools/etude/history_collect.c
 * host-deps: tools/etude/json.c utils/sha256.c
 * host-deps: tools/etude/capture.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include -I tools/etude -pthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/wait.h>
#endif
#include "compat/fixdir_host.c"
#include "etude/observe.h"
#include "observe_scope.h"

static int failures;
#define CHECK(test)                                                              \
    do {                                                                         \
        if (!(test)) {                                                           \
            fprintf(stderr, "etude_observe_hosttest:%d: %s\n", __LINE__, #test); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static struct load_process process(long pid, uint64_t creation, long parent, uint64_t cpu)
{
    struct load_process p;
    memset(&p, 0, sizeof p);
    p.id.pid = pid;
    p.id.start = creation;
    p.id.start_known = 1;
    p.parent = parent;
    p.cpu_us = cpu;
    return p;
}

/* Reused PIDs and missing ancestry cannot be charged as known outside work. */
static void accounting(void)
{
    struct load_process previous[4], p[4];
    struct load_account a = {0};
    struct load_snapshot s = {0};
    a.root.pid = 20;
    a.root.start = 20;
    a.root.start_known = 1;
    a.previous = previous;
    p[0] = process(10, 10, 1, 100);
    p[1] = process(20, 20, 10, 200);
    p[2] = process(30, 30, 20, 300);
    s.count = 3;
    etude_load_account(&a, &s, p);
    CHECK(p[0].scope == 2 && p[1].scope == 1 && p[2].scope == 1);
    p[0] = process(10, 10, 1, 110);
    p[1] = process(30, 30, 1, 330);
    p[2] = process(40, 40, 999, 400);
    etude_load_account(&a, &s, p);
    CHECK(a.outside_us == 10 && p[1].scope == 1 && p[2].scope == 0 && a.lost == 1);
    p[0] = process(10, 11, 999, 9000);
    p[1] = process(30, 30, 1, 340);
    p[2] = process(40, 40, 999, 410);
    etude_load_account(&a, &s, p);
    CHECK(a.outside_us == 10 && p[0].scope == 0 && a.lost == 2);
    p[1].cpu_us = 1;
    etude_load_account(&a, &s, p);
    CHECK(a.resets == 1 && a.outside_us == 10);
}

static void trace(char *out, size_t size, unsigned gap, int reset)
{
    int n =
        snprintf(out, size,
                 "{\"kind\":\"start\",\"schema_version\":1,\"method\":\"native-load-v1\","
                 "\"platform\":\"linux-proc-counters-v1\",\"machine_id\":\"m\",\"boot_id\":\"b\","
                 "\"run_id\":\"fixture\",\"root_pid\":1,\"root_creation\":\"1\","
                 "\"observer_pid\":2,\"observer_creation\":\"2\",\"requested_interval_us\":1000000,"
                 "\"process_limit\":4096}\n");
    for (unsigned i = 0; i < 3; i++)
        n += snprintf(
            out + n, size - (size_t)n,
            "{\"kind\":\"sample\",\"monotonic_us\":%u,\"utc_us\":%u,\"system_busy_cpu_us\":%u,"
            "\"outside_cpu_lower_us\":0,\"shared_virtualizer_cpu_lower_us\":0,"
            "\"processes\":2,\"denied\":1,\"lost\":1,\"unclassified\":1,\"overflow\":false,"
            "\"identities_sha256\":"
            "\"0000000000000000000000000000000000000000000000000000000000000000\"}\n",
            i * gap, 100 + i * gap, reset && i == 1 ? 0 : 100 + i);
    snprintf(out + n, size - (size_t)n,
             "{\"kind\":\"end\",\"utc_us\":%u,\"monotonic_us\":%u,\"samples\":3,"
             "\"complete\":true,\"owner_lost\":false}\n",
             100 + 2 * gap, 2 * gap);
}

static void imports(void)
{
    char text[4096];
    trace(text, sizeof text, 1000000, 0);
    size_t size = strlen(text);
    /* Deliberately omit a NUL after the declared bytes. */
    char *bounded = malloc(size);
    memcpy(bounded, text, size);
    char *v = etude_observe_import(bounded, size, "fixture", 100, 2000100);
    CHECK(v != NULL);
    free(v);
    CHECK(etude_observe_import(bounded, size - 1, "fixture", 100, 2000100) == NULL);
    CHECK(etude_observe_import(bounded, size, "other", 100, 2000100) == NULL);
    CHECK(etude_observe_import(bounded, size, "fixture", 99, 2000100) == NULL);
    CHECK(etude_observe_import(bounded, size, "fixture", 100, 2000101) == NULL);
    free(bounded);
    trace(text, sizeof text, 6000000, 0);
    CHECK(etude_observe_import(text, strlen(text), "fixture", 100, 12000100) == NULL);
    v = etude_observe_read(text, strlen(text), "fixture", 100, 12000100);
    CHECK(v && strstr(v, "\"comparison_available\":false"));
    free(v);
    trace(text, sizeof text, 1000000, 1);
    CHECK(etude_observe_import(text, strlen(text), "fixture", 100, 2000100) == NULL);
    v = etude_observe_read(text, strlen(text), "fixture", 100, 2000100);
    CHECK(v && strstr(v, "\"comparison_available\":false"));
    free(v);
    CHECK(etude_observe_utc("1970-01-01T00:00:01.000001Z") == 1000001);
    CHECK(etude_observe_utc("invalid") == 0);
}

/* Read a closed trace whole; return its size, or 0 when it cannot be read. */
static size_t trace_bytes(const char *path, char *bytes, size_t limit)
{
    FILE *file = fopen(path, "rb");
    size_t size = file ? fread(bytes, 1, limit - 1, file) : 0;
    int bad = !file || ferror(file) || !feof(file);
    if (file && fclose(file))
        bad = 1;
    bytes[size] = 0;
    return bad ? 0 : size;
}

#ifndef _WIN32
/* An owner that exits before its last reading leaves an incomplete trace. */
static void terminal_lost(void)
{
    int gate[2];
    CHECK(pipe(gate) == 0);
    pid_t child = fork();
    if (child == 0) {
        char c;
        close(gate[1]);
        while (read(gate[0], &c, 1) > 0)
            ;
        _exit(0);
    }
    close(gate[0]);
    struct etude_pid owner;
    struct etude_observer *o = NULL;
    const char *path = fix_path("terminal-lost");
    if (child > 0 && etude_process_state(child, &owner) == 1 && owner.start_known)
        o = etude_observe_start_terminal(path, &owner, "fixture");
    CHECK(o != NULL);
    close(gate[1]);
    int status;
    CHECK(child > 0 && waitpid(child, &status, 0) == child);
    if (!o)
        return;
    while (!etude_observe_done(o))
        poll(NULL, 0, 10);
    struct etude_reading end;
    CHECK(etude_observe_finish(o, &end) != 0);
    char *summary = etude_observe_summary(o, NULL);
    CHECK(summary && strstr(summary, "\"comparison_available\":false"));
    free(summary);
    uint64_t first, last;
    etude_observe_bounds(o, &first, &last);
    char bytes[8192];
    size_t size = trace_bytes(path, bytes, sizeof bytes);
    CHECK(size && strstr(bytes, "\"complete\":false,\"owner_lost\":true}"));
    CHECK(etude_observe_import(bytes, size, "fixture", first, last) == NULL);
    etude_observe_free(o);
}
#endif

static void terminal_native(const struct etude_pid *self)
{
    const char *path = fix_path("terminal");
    struct etude_observer *o = etude_observe_start_terminal(path, self, "fixture");
    CHECK(o != NULL);
    if (!o)
        return;
    struct etude_reading cleanup, end;
    CHECK(etude_history_now(&cleanup) == 0);
    CHECK(etude_observe_finish(o, &end) == 0 && etude_observe_done(o));
    CHECK(end.monotonic_us >= cleanup.monotonic_us);
    uint64_t first, last;
    etude_observe_bounds(o, &first, &last);
    CHECK(first <= etude_observe_utc(cleanup.utc) && last >= etude_observe_utc(end.utc));
    CHECK(etude_observe_finish(o, &end) != 0);
    char bytes[8192];
    size_t size = trace_bytes(path, bytes, sizeof bytes);
    CHECK(size && strstr(bytes, "\"complete\":true,\"owner_lost\":false}"));
    char *summary = etude_observe_import(bytes, size, "fixture", first, etude_observe_utc(end.utc));
    CHECK(summary != NULL);
    free(summary);
    etude_observe_free(o);
    o = etude_observe_start_terminal(fix_path("terminal-cancelled"), self, "fixture");
    CHECK(o != NULL);
    if (o) {
        CHECK(etude_observe_stop(o) != 0);
        summary = etude_observe_summary(o, NULL);
        CHECK(summary && strstr(summary, "\"comparison_available\":false"));
        free(summary);
        etude_observe_free(o);
    }
    path = fix_path("terminal-cancel");
    o = etude_observe_start_terminal(path, self, "fixture");
    CHECK(o != NULL);
    if (o) {
        CHECK(etude_observe_cancel(o) != 0 && etude_observe_finish(o, &end) != 0);
        etude_observe_bounds(o, &first, &last);
        size = trace_bytes(path, bytes, sizeof bytes);
        CHECK(size && strstr(bytes, "\"complete\":false,\"owner_lost\":false}"));
        CHECK(etude_observe_import(bytes, size, "fixture", first, last) == NULL);
        etude_observe_free(o);
    }
#ifndef _WIN32
    terminal_lost();
#endif
}

static void native(void)
{
    struct etude_pid self;
#ifdef _WIN32
    long pid = (long)GetCurrentProcessId();
#else
    long pid = (long)getpid();
#endif
    CHECK(etude_process_state(pid, &self) == 1 && self.start_known);
    terminal_native(&self);
    struct etude_pid wrong = self;
    wrong.start++;
    CHECK(etude_observe_start(fix_path("wrong"), &wrong, "fixture") == NULL);
    struct etude_observer *o = etude_observe_start(fix_path("native"), &self, "fixture");
    CHECK(o != NULL);
    if (!o)
        return;
    CHECK(etude_observe_stop(o) == 0 && etude_observe_done(o));
    char *summary = etude_observe_summary(o, NULL);
    CHECK(summary && strstr(summary, "\"comparison_available\":true") &&
          strstr(summary, "\"non_owned_cpu_upper_bound_us\":{\"value\":null"));
    free(summary);
    etude_observe_free(o);
    o = etude_observe_start(fix_path("cancelled"), &self, "fixture");
    CHECK(o != NULL);
    if (o) {
        CHECK(etude_observe_cancel(o) != 0);
        summary = etude_observe_summary(o, NULL);
        CHECK(summary && strstr(summary, "\"comparison_available\":false"));
        free(summary);
        etude_observe_free(o);
    }
}

#include "compat/observe_sequence_fixture.c"

int main(int argc, char **argv)
{
    sequence_program = argv[0];
    int role = sequence_fixture_role(argc, argv);
    if (role >= 0)
        return role;
    sequence_fixture();
    accounting();
    imports();
    native();
    if (!failures) {
        fix_passed();
        puts("etude_observe_hosttest: pass");
    }
    return failures != 0;
}
