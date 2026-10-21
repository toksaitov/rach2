/*
 * clock_scenario.c: the scenario of the clock case (../machine.clock.json),
 * clockloadmusl's comparison of the guest's clocks with the host's across
 * thirty stops of the machine's QEMU, against the reference clockpeer serves.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"

/* The cycles of the suspension `stalls` that the manifest declares. */
#define STALLS 30

/* The start of each line clockloadmusl prints. */
#define LINE "CLOCKLOAD "

/* The room for the rest of one of its lines. */
#define LINE_LEN 160

static const char *const ids[] = {"ready", "stopped", "monotonic", "realtime", "ok", NULL};

/**
 * Wait for the next line of clockloadmusl's on the console of `m` after
 * `*at`, copy the rest of it after `CLOCKLOAD ` into `buf`, and move `*at`
 * past its end. @return 1 when a whole line came.
 */
static int next_line(struct lc *l, struct etude_case_machine *m, uint64_t *at, char buf[LINE_LEN])
{
    uint64_t start;

    if (!lc_wait(l, m, LINE, at, NULL))
        return 0;
    start = *at - (sizeof(LINE) - 1);
    if (!lc_wait(l, m, "\n", at, NULL))
        return 0;
    if (!lc_text(l, "clock.1.console", LINE, start, buf, LINE_LEN)) {
        lc_halt(l, ETUDE_FAILED, "the read of the console's evidence");
        return 0;
    }
    return 1;
}

/** Say whether `last`, the line that ended clockloadmusl, is the failure of a
 * clock's comparison: a drift, or an error interval that crosses the bound. */
static int verdict_failed(const char *last)
{
    return strcmp(last, "FAIL (clock drift)") == 0 ||
           strcmp(last, "FAIL (reference uncertainty)") == 0;
}

/**
 * Report assertion `id` of the clock whose comparison clockloadmusl printed
 * as `said`, or did not when it is empty: not run when it did not, and else
 * failed when `failed`, with `last`, the line that ended the program.
 */
static void comparison(struct lc *l, const char *id, const char *said, int failed, const char *last)
{
    char why[2 * LINE_LEN + 64];

    if (said[0] == '\0') {
        snprintf(why, sizeof(why), "clockloadmusl compared no %s clock: it said CLOCKLOAD %s", id,
                 last);
        lc_notrun(l, id, why);
        return;
    }
    snprintf(why, sizeof(why), "clockloadmusl says CLOCKLOAD %s, then CLOCKLOAD %s", said, last);
    lc_assert(l, id, !failed, why);
}

/** Print on stderr the time the host held the machine stopped in `n` stops. */
static void say_stops(const struct etude_stop *stops, unsigned n)
{
    uint64_t held = 0;
    unsigned i;

    for (i = 0; i < n; i++)
        held += stops[i].resumed_us - stops[i].stopped_us;
    fprintf(stderr, "clock_scenario: stops: %u, held %" PRIu64 " us in all\n", n, held);
}

/**
 * Run clockloadmusl at the debug shell of `m`, read to `*at`, with the
 * reference's `port`, and wait for its first line: assertion `ready`. A
 * first line other than `READY` is left in `last`, as the line that ended
 * the program; after `READY`, `last` is empty.
 *
 * @return 1 when it said it is ready.
 */
static int start(struct lc *l, struct etude_case_machine *m, const char *port, uint64_t *at,
                 char last[LINE_LEN])
{
    char command[64], what[96];
    int ready;

    snprintf(command, sizeof(command), "exec /musl/clockloadmusl %s\n", port);
    if (!lc_step(l, "start") || !lc_type(l, m, command) || !next_line(l, m, at, last))
        return 0;
    ready = strcmp(last, "READY") == 0;
    snprintf(what, sizeof(what), "clockloadmusl said CLOCKLOAD %.30s", last);
    lc_assert(l, "ready", ready, what);
    if (!ready)
        lc_halt(l, ETUDE_FAILED, "the start of clockloadmusl");
    else
        last[0] = '\0';
    return ready;
}

/** Stop and resume the QEMU of `m` thirty times, as the suspension `stalls`
 * declares: assertion `stopped`. */
static void stall(struct lc *l, struct etude_case_machine *m)
{
    struct etude_stop stops[STALLS];
    enum etude_status st;
    unsigned done = 0;
    char what[96];

    if (!lc_step(l, "stall"))
        return;
    st = etude_suspend(m, "stalls", stops, STALLS, &done);
    say_stops(stops, done < STALLS ? done : STALLS);
    if (st != ETUDE_OK) {
        snprintf(what, sizeof(what), "the stops, after %u of %d cycles", done, STALLS);
        lc_halt(l, st, what);
    }
    if (st != ETUDE_STOPPED)
        lc_assert(l, "stopped", st == ETUDE_OK && done == STALLS,
                  st == ETUDE_OK ? "the stops ran fewer cycles than declared" : l->why);
}

/**
 * Type the newline that ends clockloadmusl's load on `m`, and read its lines
 * up to its last, `CLOCKLOAD OK` or its first `CLOCKLOAD FAIL`, which is
 * kept in `last`: assertions `monotonic` and `realtime`. @return 1 when the
 * last line came.
 */
static int finish(struct lc *l, struct etude_case_machine *m, uint64_t *at, char last[LINE_LEN])
{
    char line[LINE_LEN], mono[LINE_LEN] = "", real[LINE_LEN] = "";

    if (!lc_step(l, "finish") || !lc_type(l, m, "\n"))
        return 0;
    while (next_line(l, m, at, line)) {
        if (strncmp(line, "monotonic error ", 16) == 0) {
            strcpy(mono, line);
        } else if (strncmp(line, "realtime error ", 15) == 0) {
            strcpy(real, line);
        } else if (strcmp(line, "OK") == 0 || strncmp(line, "FAIL", 4) == 0) {
            strcpy(last, line);
            comparison(l, "monotonic", mono, real[0] == '\0' && verdict_failed(last), last);
            comparison(l, "realtime", real, verdict_failed(last), last);
            return 1;
        }
    }
    return 0;
}

/**
 * Start the clock reference on the case's port, boot the test image, and run
 * clockloadmusl at the debug shell with the port. Once it says it is ready,
 * stop and resume the machine's QEMU thirty times, then end the program's
 * load and read its comparisons of the monotonic and the real-time clock
 * with the host's monotonic interval. The program judges each clock; the
 * scenario times nothing.
 */
int etude_scenario(struct etude_case *c)
{
    struct lc l;
    struct etude_case_service *peer = NULL;
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    char port[12], what[96], last[LINE_LEN] = "";
    char *args[2];
    enum etude_status st;
    uint64_t at = 0;

    lc_begin(&l, c, ids);
    snprintf(port, sizeof(port), "%u", etude_port(c, 0));
    args[0] = port;
    args[1] = NULL;
    if (lc_step(&l, "serve") && (st = etude_service(c, "reference", args, &peer, &end)) != ETUDE_OK)
        lc_halt(&l, st, "the start of clockpeer");
    if (lc_ok(&l))
        m = lc_shell(&l, "boot", "clock", &at);
    if (m != NULL && start(&l, m, port, &at, last)) {
        stall(&l, m);
        if (lc_ok(&l))
            finish(&l, m, &at, last);
    }
    if (last[0] != '\0') {
        snprintf(what, sizeof(what), "clockloadmusl ended with CLOCKLOAD %.30s", last);
        lc_assert(&l, "ok", strcmp(last, "OK") == 0, what);
    }
    if (m != NULL)
        lc_quit(&l, m, NULL);
    if (peer != NULL)
        etude_service_stop(peer, &end);
    return lc_finish(&l);
}
