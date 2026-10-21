/*
 * memory_scenario.c: the scenario of machine.memory, the kernel's account of
 * physical memory. Cases `size-MIB` boot the test image with MIB MiB and
 * check the four numbers of the memory report against the map the kernel
 * printed and the size QEMU gave; `memprobe-MIB` and `framefail-MIB` run
 * memprobe at the two ends of the range, the second with every frame
 * allocation of a fork and an exec failed in turn; `strings` sweeps the
 * kernel's copy, fill and move routines; `refused` boots the recovery kernel
 * in 8 MiB, which it must refuse by name; and `stackoverrun-fork` and
 * `stackoverrun-boot` run a kernel stack out.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The KiB of QEMU's fixed holes in the memory of a PC: the kilobyte at
 * 0x9FC00, the 384 KiB below 1 MiB and the 128 KiB at the end of low RAM. */
#define HOLES_KIB 513

/* The partial frames at the ends of the map's ranges, in KiB, by which the
 * frames the kernel manages may fall short of usable less reserved. */
#define PARTIAL_KIB 32

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Read the number of KiB after `name` and a space in `line`, as the memory
 * report gives it, `usable 130420 KiB`. @return 1 when it is there.
 */
static int kib(const char *line, const char *name, uint64_t *v)
{
    char key[32];
    const char *p;

    snprintf(key, sizeof(key), "%s ", name);
    p = strstr(line, key);
    return p != NULL && sscanf(p + strlen(key), "%" SCNu64 " KiB", v) == 1;
}

/**
 * Sum the KiB of the ranges that the kernel's lines `memory: map: START-END
 * available` give in console text `w`, each in hexadecimal.
 */
static uint64_t available_kib(const char *w)
{
    const char *p = w;
    uint64_t start, end, sum = 0;

    while ((p = strstr(p, "memory: map: ")) != NULL) {
        p += strlen("memory: map: ");
        if (sscanf(p, "0x%" SCNx64 "-0x%" SCNx64, &start, &end) == 2 && end > start &&
            strncmp(strchr(p, ' ') != NULL ? strchr(p, ' ') : p, " available", 10) == 0)
            sum += (end - start) / 1024;
    }
    return sum;
}

/**
 * Run case `size-MIB`: boot with `mib` MiB and check the report of the
 * console `w` read to the shell: assertions `report`, `map`, `size` and
 * `managed`.
 */
static void size(struct lc *l, uint64_t mib)
{
    struct etude_case_machine *m;
    uint64_t at, usable, unreachable, reserved, managed, map;
    const char *line;
    char why[200], *w;
    int four;

    if ((m = lc_shell(l, "boot", "fixture", &at)) == NULL ||
        (w = lc_console(l, "fixture", 0, at)) == NULL)
        return;
    line = strstr(w, "memory: usable ");
    four = line != NULL && kib(line, "usable", &usable) && kib(line, "unreachable", &unreachable) &&
           kib(line, "reserved", &reserved) && kib(line, "managed", &managed);
    lc_assert(l, "report", four, "the kernel gave no report of four numbers");
    if (four) {
        map = available_kib(w);
        snprintf(why, sizeof(why),
                 "usable %" PRIu64 " and unreachable %" PRIu64 " KiB are not the map's %" PRIu64
                 " KiB",
                 usable, unreachable, map);
        lc_assert(l, "map", usable + unreachable == map, why);
        snprintf(why, sizeof(why),
                 "usable %" PRIu64 ", unreachable %" PRIu64 " and the holes are not %" PRIu64
                 " KiB",
                 usable, unreachable, mib * 1024);
        lc_assert(l, "size", usable + unreachable + HOLES_KIB == mib * 1024, why);
        snprintf(why, sizeof(why),
                 "managed %" PRIu64 " KiB is not usable %" PRIu64 " less reserved %" PRIu64,
                 managed, usable, reserved);
        lc_assert(l, "managed",
                  usable >= reserved && managed <= usable - reserved &&
                      managed + PARTIAL_KIB > usable - reserved,
                  why);
    }
    free(w);
    lc_quit(l, m, NULL);
}

/**
 * Run memprobe with `argument` at the debug shell: assertions `exit` and
 * `said-ok`, and with `failing` set `failed-on-purpose`, the kernel's record
 * that a frame allocation failed as rach2.framefail asks.
 */
static void memprobe(struct lc *l, const char *argument, int failing)
{
    struct etude_case_machine *m;
    uint64_t at;
    char command[64], *w;

    snprintf(command, sizeof(command), "exec /memprobe %s\r", argument);
    if ((m = lc_shell(l, "probe", "fixture", &at)) == NULL)
        return;
    w = lc_run(l, m, "fixture", command, &at, "exit");
    if (w != NULL) {
        lc_assert(l, "said-ok",
                  strstr(w, "MEMPROBE OK") != NULL && strstr(w, "MEMPROBE FAIL") == NULL,
                  "memprobe did not say MEMPROBE OK alone");
        if (failing)
            lc_assert(l, "failed-on-purpose",
                      strstr(w, "frame allocation failed on purpose") != NULL,
                      "the kernel failed no frame allocation on purpose");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Say whether a record of the string sweep in console text `w`, a line with
 * `strings: `, says that a routine was wrong.
 */
static int sweep_wrong(const char *w)
{
    const char *p = w, *end, *bad;

    while ((p = strstr(p, "strings: ")) != NULL) {
        end = strchr(p, '\n');
        if (end == NULL)
            end = p + strlen(p);
        bad = strstr(p, "wrong");
        if (bad != NULL && bad < end)
            return 1;
        p = end;
    }
    return 0;
}

/**
 * Run case `strings`: the boot sweeps the string routines before the debug
 * shell starts, and says so. Assertions `swept` and `never-wrong`, judged on
 * the console up to the shell.
 */
static void strings(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at;
    char *w;

    if ((m = lc_shell(l, "boot", "fixture", &at)) == NULL)
        return;
    if ((w = lc_console(l, "fixture", 0, at)) != NULL) {
        lc_assert(l, "swept", strstr(w, "strings: swept") != NULL,
                  "the kernel did not report a sweep of the string routines");
        lc_assert(l, "never-wrong", !sweep_wrong(w), "the kernel found a string routine wrong");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Run case `refused`: the recovery kernel in 8 MiB says `memory: refused`
 * and stops, with no fault. Assertions `refused` and `no-fault`.
 */
static void refused(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "memory: refused", &at, "refused") ||
        !lc_wait(l, m, "memory: too little for this kernel", &at, NULL) || !lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-fault", "vm: fault", 0, 0);
}

/**
 * Run case `stackoverrun-WHERE`: the descent announces its pid, a record of
 * the fault names the same pid, the kernel panics because a kernel stack ran
 * out, and the debug shell never starts. The panic is recorded as the boot's
 * end, which `etude.fault` judges against the machine's declared fault.
 * Assertions `descended`, `named`, `panicked` and `no-shell`.
 */
static void stackoverrun(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0, start;
    char named[96], *w;
    int pid = -1;
    const char *p;
    enum etude_status st;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL)
        return;
    start = at;
    if (!lc_wait(l, m, "kstack: overrun: pid ", &at, "descended") ||
        !lc_wait(l, m, " descends its stack", &at, NULL) ||
        (w = lc_console(l, "fixture", start, at)) == NULL)
        return;
    p = strstr(w, "kstack: overrun: pid ");
    if (p != NULL)
        pid = atoi(p + strlen("kstack: overrun: pid "));
    free(w);
    snprintf(named, sizeof(named), "pid %d ran past the end of its kernel stack", pid);
    if (!lc_wait(l, m, "a kernel stack ran out", &at, "panicked"))
        return;
    st = etude_fault(m, ETUDE_END_PANIC);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "the record of the kernel's panic");
        return;
    }
    if (lc_quit(l, m, NULL)) {
        lc_find(l, m, "named", named, 0, 1);
        lc_find(l, m, "no-shell", "Type 'help'", 0, 0);
    }
}

int etude_scenario(struct etude_case *c)
{
    static const char *const size_ids[] = {"report", "map", "size", "managed", NULL};
    static const char *const memprobe_ids[] = {"exit", "said-ok", NULL};
    static const char *const framefail_ids[] = {"exit", "said-ok", "failed-on-purpose", NULL};
    static const char *const strings_ids[] = {"swept", "never-wrong", NULL};
    static const char *const refused_ids[] = {"refused", "no-fault", NULL};
    static const char *const overrun_ids[] = {"descended", "panicked", "named", "no-shell", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strncmp(name, "size-", 5) == 0) {
        lc_begin(&l, c, size_ids);
        size(&l, strtoull(name + 5, NULL, 10));
    } else if (strcmp(name, "memprobe-64") == 0 || strcmp(name, "memprobe-4096") == 0) {
        lc_begin(&l, c, memprobe_ids);
        memprobe(&l, strcmp(name, "memprobe-64") == 0 ? "24" : "1024", 0);
    } else if (strncmp(name, "framefail-", 10) == 0) {
        lc_begin(&l, c, framefail_ids);
        memprobe(&l, "fail", 1);
    } else if (strcmp(name, "strings") == 0) {
        lc_begin(&l, c, strings_ids);
        strings(&l);
    } else if (strcmp(name, "refused") == 0) {
        lc_begin(&l, c, refused_ids);
        refused(&l);
    } else if (strncmp(name, "stackoverrun-", 13) == 0) {
        lc_begin(&l, c, overrun_ids);
        stackoverrun(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
