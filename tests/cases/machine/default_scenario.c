/*
 * default_scenario.c: the scenario of the machine cases of guest.default,
 * the probes that need what a guest case of etude-guest cannot give them:
 * the designated init's role, the console as their terminal or their
 * standard input, no HOME, or a clock reference on the host. Each case
 * boots the test image, runs its probe from the kernel's debug shell, as
 * that shell's transient init on the console, and judges the record with
 * which the kernel hands the shell back. The console is a virtio console's
 * port, as the cases' machines declare, so that the probes of the terminal
 * run on it; the suite `console` reads the UART's. The case `timing` judges
 * the kernel's record of its clock's period accounting at boot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* A probe of a case: the last part of the case's identity, and what the
 * debug shell is given to run it; `%u` in it is the clock reference's port. */
struct probe {
    const char *name;
    const char *command;
};

static const struct probe probes[] = {
    {"survivorprobe", "exec /survivorprobe\r"}, {"orphanprobe", "exec /orphanprobe\r"},
    {"jobprobe", "exec /jobprobe\r"},           {"pollprobe", "exec /pollprobe\r"},
    {"devprobe", "exec /devprobe\r"},           {"fdcallmusl", "exec /musl/fdcallmusl\r"},
    {"ishmusl", "exec /musl/ishmusl\r"},        {"clockmusl", "exec /musl/clockmusl %u\r"},
    {"threadmusl", "exec /musl/threadmusl\r"},
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Run probe `p` at the debug shell and judge its window, from the typing of
 * its command to the record that hands the shell back: assertion `exit`,
 * and for clockmusl `reference`.
 */
static void run_probe(struct lc *l, const struct probe *p)
{
    struct etude_case_machine *m;
    uint64_t at;
    unsigned port = 0;
    pid_t ref = -1;
    char command[96], *w;
    int clock = strcmp(p->name, "clockmusl") == 0;

    if (clock && (ref = lc_clockref_start(&port)) < 0) {
        lc_halt(l, ETUDE_FAILED, "the clock reference's socket");
        return;
    }
    snprintf(command, sizeof(command), p->command, port);
    if ((m = lc_shell(l, "probe", "probe", &at)) != NULL) {
        w = lc_run(l, m, "probe", command, &at, "exit");
        if (w != NULL && clock)
            lc_assert(l, "reference", lc_clockref_said(w, strlen(w)),
                      "clockmusl gave no result of its comparison with the reference");
        free(w);
        lc_quit(l, m, NULL);
    }
    lc_clockref_stop(ref);
}

/**
 * Judge the record the kernel prints once at boot of its clock's period
 * accounting, before the debug shell: assertion `timing` holds when it is at
 * level 6.
 */
static void timing(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at;
    char *w;
    int level;

    if ((m = lc_shell(l, "probe", "probe", &at)) == NULL)
        return;
    w = lc_console(l, "probe", 0, at);
    if (w != NULL) {
        level = lc_timing_level(w, strlen(w));
        lc_assert(l, "timing", level == 6,
                  level == 3 ? "the kernel's timing record is at level 3: its check failed"
                             : "the kernel said nothing about time");
        free(w);
    }
    lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const probe_ids[] = {"exit", NULL};
    static const char *const clock_ids[] = {"exit", "reference", NULL};
    static const char *const timing_ids[] = {"timing", NULL};
    const char *name = case_name(c);
    struct lc l;
    size_t i;

    if (strcmp(name, "timing") == 0) {
        lc_begin(&l, c, timing_ids);
        timing(&l);
        return lc_finish(&l);
    }
    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
        if (strcmp(name, probes[i].name) == 0) {
            lc_begin(&l, c, strcmp(name, "clockmusl") == 0 ? clock_ids : probe_ids);
            run_probe(&l, &probes[i]);
            return lc_finish(&l);
        }
    return 1;
}
