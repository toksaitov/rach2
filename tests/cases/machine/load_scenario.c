/*
 * load_scenario.c: the scenario of the load cases (../machine.load.json).
 * loadprobe loads the machine from the kernel's debug shell while six lines
 * are typed at its console, each once its reader has said that it took the
 * last, and its report is read from the console.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_report.h"

/* The debug shell's banner, after which it reads commands. */
#define SHELL_READY "Type 'help' for available commands."

/* The reader's line for each line it took, and the start of the probe's
 * line of the longest delays. */
#define READ_LINE  "LOADPROBE read"
#define DELAY_LINE "LOADPROBE timer"

/* The probe's record of its loads, which it writes as a pass, a failure or
 * an assertion not run, and the record of the pass. */
#define STARTED      " started "
#define STARTED_PASS "@etude 1 assert started pass"

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Copy the probe's line of the longest delays from the console of boot 1 of
 * the machine `guest`, the bytes before `end`, to standard output, for a
 * person to read.
 */
static void print_delays(struct etude_case *c, uint64_t end)
{
    const char *path = etude_evidence(c, "guest.1.console");
    char line[512];
    uint64_t at = 0;
    FILE *f;

    if (path == NULL || (f = fopen(path, "rb")) == NULL)
        return;
    while (at < end && fgets(line, sizeof(line), f) != NULL) {
        at += strlen(line);
        if (strncmp(line, DELAY_LINE, strlen(DELAY_LINE)) == 0) {
            line[strcspn(line, "\r\n")] = '\0';
            printf("load: %s\n", line);
        }
    }
    fclose(f);
}

/**
 * Boot the test image with the disk of the case, run loadprobe at the debug
 * shell with the loads the case names, type each of six lines once the
 * reader has said that it took the one before, take the probe's report and
 * end the machine with QMP's quit. A probe that did not start its loads
 * reads no line, so its report is taken without them.
 */
int etude_scenario(struct etude_case *c)
{
    static const char *const lines[] = {"one\r", "two\r", "three\r", "four\r", "five\r", "six\r"};
    const char *command =
        strcmp(case_name(c), "polled") == 0 ? "exec /loadprobe Wnr\r" : "exec /loadprobe\r";
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status quit;
    char *reply = NULL;
    uint64_t typed, at, pass;
    size_t i;

    if (etude_step(c, "boot") != ETUDE_OK || etude_boot(c, "guest", NULL, &m, &end) != ETUDE_OK ||
        etude_console_wait(m, SHELL_READY, 0, &typed) != ETUDE_OK)
        return 1;
    if (etude_step(c, "load") != ETUDE_OK || etude_type(m, command) != ETUDE_OK ||
        etude_console_wait(m, STARTED, typed, &at) != ETUDE_OK ||
        etude_console_wait(m, "\n", at, &at) != ETUDE_OK)
        return 1;
    if (etude_console_find(m, STARTED_PASS, typed, &pass) == ETUDE_OK)
        for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
            if (etude_type(m, lines[i]) != ETUDE_OK ||
                etude_console_wait(m, READ_LINE, at, &at) != ETUDE_OK)
                return 1;
    if (console_report(c, m, "guest", 1, typed, &at) != ETUDE_OK)
        return 1;
    print_delays(c, at);
    quit = etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    if (quit == ETUDE_STOPPED)
        return 1;
    return etude_wait_end(m, &end) == ETUDE_OK ? 0 : 1;
}
