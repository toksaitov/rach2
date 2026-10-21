/*
 * probe_report.h: etude's report records for the guest programs: the
 * freestanding probes, which include it after probe.h, and the musl
 * programs, which write through the C library's write(2) and give `environ`
 * to `report_case`. tools/etude/include/etude/report.h defines the records
 * and tools/etude/Readme.md their contract.
 *
 * A probe begins its report with `report_begin`, gives each assertion of its
 * path once with `report_pass`, `report_fail` or `report_notrun` and each
 * group's branch with `report_branch`, and ends with `report_end`. Each
 * record is one write(2) of at most ETUDE_REPORT_LINE_MAX bytes, so that no
 * kernel record falls inside it on the console; a detail too long for it is
 * cut, and a byte of it outside printable ASCII becomes '?'.
 *
 * A probe that ends at its first failed check plans its path, the
 * assertions it checks in order, with `report_plan`, reports each that held
 * with `report_held`, and calls `report_stop` before it exits at a failure:
 * that assertion fails, those after it are not run, and the report ends.
 * Only the process that began the report writes these records; a forked
 * child that stops writes none. A probe that goes on past a failed check
 * reports that assertion with `report_missed` and continues.
 *
 * A probe that counts its failures and goes on divides its checks into
 * parts, one assertion each. `report_part` begins a part, ending the one
 * before it; `report_miss` records that a check of the open part failed,
 * keeping the first reason; and `report_finish` ends the open part and the
 * report. A part is held when none of its checks failed.
 *
 * The records are written beside the probe's own lines, such as "RNGPROBE
 * OK", which tests/run's suites read and etude does not.
 */
#ifndef PROBE_REPORT_H
#define PROBE_REPORT_H

#include "../tools/etude/include/etude/report.h"

#if defined __STDC_HOSTED__ && __STDC_HOSTED__
#include <unistd.h>
#define report_write(line, n) ((void)write(1, (line), (size_t)(n)))
#define report_getpid()       ((int)getpid())
#else
#define report_write(line, n) ((void)syscall3(SYS_WRITE, 1, (long)(line), (n)))
#define report_getpid()       ((int)syscall0(SYS_GETPID))
#endif

/* The assert, notrun and branch records written so far, which `end` counts. */
static int report_records;

/* The planned path: its assertions, the next of them, and the process that
 * began the report; and whether the report has ended. */
static const char *report_path[64];
static int report_path_len, report_path_at, report_owner, report_ended;

/** Append `s` to the record `line`, which has `*n` bytes, as room allows. */
static __inline__ void report_put(char *line, int *n, const char *s)
{
    while (*s != '\0' && *n < ETUDE_REPORT_LINE_MAX - 1) {
        line[(*n)++] = *s >= ' ' && *s <= '~' ? *s : '?';
        s++;
    }
}

/** Write the record of `verb` and the fields `a`, `b` and `c`, each NULL for
 * none, with one write(2). */
static __inline__ void report_record(const char *verb, const char *a, const char *b, const char *c)
{
    char line[ETUDE_REPORT_LINE_MAX];
    const char *field[3];
    int n = 0, i;

    field[0] = a;
    field[1] = b;
    field[2] = c;
    report_put(line, &n, ETUDE_REPORT_PREFIX);
    report_put(line, &n, verb);
    for (i = 0; i < 3; i++) {
        if (field[i] != 0) {
            report_put(line, &n, " ");
            report_put(line, &n, field[i]);
        }
    }
    line[n++] = '\n';
    report_write(line, n);
}

/**
 * Return the identity of the case the probe runs: ETUDE_CASE of the
 * environment `envp`, which etude sets, or `own` without it, as in a run by
 * tests/run.
 */
static __inline__ const char *report_case(char **envp, const char *own)
{
    static const char name[] = "ETUDE_CASE=";
    int i, k;

    for (i = 0; envp[i] != 0; i++) {
        k = 0;
        while (name[k] != '\0' && envp[i][k] == name[k]) {
            k++;
        }
        if (name[k] == '\0' && envp[i][k] != '\0') {
            return envp[i] + k;
        }
    }
    return own;
}

/** Begin the report of the case `case_id`. */
static __inline__ void report_begin(const char *case_id)
{
    report_owner = report_getpid();
    report_record(ETUDE_REPORT_BEGIN, case_id, 0, 0);
}

/** Report that the assertion `id` held. */
static __inline__ void report_pass(const char *id)
{
    report_record(ETUDE_REPORT_ASSERT, id, ETUDE_REPORT_PASS, 0);
    report_records++;
}

/** Report that the assertion `id` did not hold; `detail` says what was seen. */
static __inline__ void report_fail(const char *id, const char *detail)
{
    report_record(ETUDE_REPORT_ASSERT, id, ETUDE_REPORT_FAIL, detail);
    report_records++;
}

/** Report that the assertion `id` could not be evaluated, for `reason`. */
static __inline__ void report_notrun(const char *id, const char *reason)
{
    report_record(ETUDE_REPORT_NOTRUN, id, reason, 0);
    report_records++;
}

/** Report that the alternative `alternative` of the group `group` ran. */
static __inline__ void report_branch(const char *group, const char *alternative)
{
    report_record(ETUDE_REPORT_BRANCH, group, alternative, 0);
    report_records++;
}

/** Write `n`, which is not negative, in decimal into `buf` of 12 bytes, and
 * return `buf`. */
static __inline__ char *report_decimal(char *buf, int n)
{
    char digits[12];
    int i = 0, k = 0;

    do {
        digits[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0 && i < 11);
    while (i > 0) {
        buf[k++] = digits[--i];
    }
    buf[k] = '\0';
    return buf;
}

/** End the report with the count of its records. */
static __inline__ void report_end(void)
{
    char count[12];

    report_record(ETUDE_REPORT_END, report_decimal(count, report_records), 0, 0);
    report_ended = 1;
}

/** Add the assertion `id` to the end of the planned path. */
static __inline__ void report_plan(const char *id)
{
    report_path[report_path_len++] = id;
}

/** Report that the path's next assertion held. */
static __inline__ void report_held(void)
{
    report_pass(report_path[report_path_at++]);
}

/** Report that the path's next assertion did not hold, with `why`, in a
 * probe that goes on past a failed check. */
static __inline__ void report_missed(const char *why)
{
    report_fail(report_path[report_path_at++], why);
}

/** In the process that began the report, report that the path's next
 * assertion failed with `why`, that those after it were not run, and the
 * report's end; after the end, nothing. */
static __inline__ void report_stop(const char *why)
{
    if (report_owner == 0 || report_ended || report_getpid() != report_owner) {
        return;
    }
    if (report_path_at < report_path_len) {
        report_fail(report_path[report_path_at++], why);
    }
    while (report_path_at < report_path_len) {
        report_notrun(report_path[report_path_at++], "an earlier check failed");
    }
    report_end();
}

/* The open part's assertion, whether a check of it failed, and the first
 * reason given. */
static const char *report_part_id;
static int report_part_failed;
static char report_part_why[ETUDE_REPORT_LINE_MAX];

/** In the process that began the report, report the open part's assertion:
 * held unless one of its checks failed. */
static __inline__ void report_part_end(void)
{
    if (report_part_id == 0 || report_getpid() != report_owner) {
        return;
    }
    if (report_part_failed) {
        report_fail(report_part_id, report_part_why);
    } else {
        report_pass(report_part_id);
    }
    report_part_id = 0;
}

/** End the open part, and begin the part whose assertion is `id`. */
static __inline__ void report_part(const char *id)
{
    report_part_end();
    report_part_id = id;
    report_part_failed = 0;
    report_part_why[0] = '\0';
}

/** Record that a check of the open part failed; `why` says which, and the
 * first reason of the part is the one reported, without the spaces at its
 * ends, which a record's detail cannot have. */
static __inline__ void report_miss(const char *why)
{
    int n = 0;

    if (report_part_failed) {
        return;
    }
    report_part_failed = 1;
    while (*why == ' ') {
        why++;
    }
    report_put(report_part_why, &n, why);
    while (n > 0 && report_part_why[n - 1] == ' ') {
        n--;
    }
    if (n == 0) {
        report_put(report_part_why, &n, "a check failed");
    }
    report_part_why[n] = '\0';
}

/** In the process that began the report, end the open part and the report;
 * after the end, nothing. */
static __inline__ void report_finish(void)
{
    if (report_owner == 0 || report_ended || report_getpid() != report_owner) {
        return;
    }
    report_part_end();
    report_end();
}

#endif
