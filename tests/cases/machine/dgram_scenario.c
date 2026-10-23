/*
 * dgram_scenario.c: the scenario of the dgram case (../machine.dgram.json),
 * dgramprobe at the debug shell of a machine whose card forwards a port of the
 * case's to the guest's port 7502, and the probe's broadcast found in the
 * capture of that card.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_report.h"

/* The debug shell's banner, after which it reads commands. */
#define SHELL_READY "Type 'help' for available commands."

/* The probe's line when it waits for the datagram from outside, and the
 * start of its line when a check failed. */
#define PROBE_OUTSIDE "DGRAMPROBE OUTSIDE"
#define PROBE_FAIL    "DGRAMPROBE FAIL: "

/**
 * Wait on the console of `m`, boot 1 of the machine `guest`, after byte
 * `from` for the probe's line that it waits for the datagram from outside,
 * or for its line that a check failed before it, whichever comes first.
 *
 * @return 1 for the first, 0 for the second, and -1 when a wait did not
 *         complete or the console's file could not be read.
 */
static int wait_outside(struct etude_case *c, struct etude_case_machine *m, uint64_t from)
{
    const char *given = etude_evidence(c, "guest.1.console");
    char path[4096], line[4096];
    uint64_t at;
    size_t want, got;
    FILE *f;

    if (given == NULL || strlen(given) >= sizeof(path))
        return -1;
    strcpy(path, given);
    for (;;) {
        if (etude_console_wait(m, "\n", from, &at) != ETUDE_OK)
            return -1;
        want = at - from < sizeof(line) - 1 ? (size_t)(at - from) : sizeof(line) - 1;
        if ((f = fopen(path, "rb")) == NULL)
            return -1;
        got = fseek(f, (long)from, SEEK_SET) == 0 ? fread(line, 1, want, f) : 0;
        fclose(f);
        if (got != want) {
            fprintf(stderr, "dgram_scenario: guest.1.console: the console's file cannot be read\n");
            return -1;
        }
        line[got] = '\0';
        if (strstr(line, PROBE_OUTSIDE) != NULL)
            return 1;
        if (strstr(line, PROBE_FAIL) != NULL)
            return 0;
        from = at;
    }
}

/**
 * Boot the test image with the card the case names, run dgramprobe at the
 * debug shell, and when it says that it waits, send it a datagram from the
 * host through the forward; take its report from the console. Then power
 * the machine off at the debug shell, so that the capture is whole, and
 * require the probe's broadcast to 255.255.255.255 at port 7500 in it,
 * which the probe cannot see from inside the guest.
 */
int etude_scenario(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    char port[12];
    char *send[5], *check[8];
    const char *capture;
    enum etude_status st;
    uint64_t typed, at;
    int waits;

    if (etude_step(c, "boot") != ETUDE_OK || etude_boot(c, "guest", NULL, &m, &end) != ETUDE_OK ||
        etude_console_wait(m, SHELL_READY, 0, &typed) != ETUDE_OK)
        return 1;
    if (etude_step(c, "dgramprobe") != ETUDE_OK ||
        etude_type(m, "exec /dgramprobe\r") != ETUDE_OK || (waits = wait_outside(c, m, typed)) < 0)
        return 1;
    if (waits) {
        snprintf(port, sizeof(port), "%u", etude_port(c, 0));
        send[0] = "hostpeer";
        send[1] = "send";
        send[2] = port;
        send[3] = "outsider";
        send[4] = NULL;
        if (etude_host(c, send, "hostpeer-send.out", &end) != ETUDE_OK)
            return 1;
        if (end.end != ETUDE_END_EXITED || end.value != 0)
            fprintf(stderr, "dgram_scenario: hostpeer did not send the datagram from outside\n");
    }
    if (console_report(c, m, "guest", 1, typed, &at) != ETUDE_OK)
        return 1;

    if (etude_step(c, "poweroff") != ETUDE_OK ||
        etude_console_wait(m, SHELL_READY, at, &at) != ETUDE_OK ||
        etude_type(m, "poweroff\r") != ETUDE_OK || etude_wait_end(m, &end) != ETUDE_OK)
        return 1;
    etude_assert(c, "powered-off", end.end == ETUDE_END_SHUTDOWN,
                 "the machine did not power off at the debug shell's command");

    if (etude_step(c, "broadcast") != ETUDE_OK || (capture = etude_capture(m, "net0")) == NULL)
        return 1;
    check[0] = "pcapcheck";
    check[1] = "bcast";
    check[2] = (char *)capture;
    check[3] = "--port";
    check[4] = "7500";
    check[5] = "--dst-ip";
    check[6] = "255.255.255.255";
    check[7] = NULL;
    st = etude_host(c, check, "pcapcheck.out", &end);
    if (st != ETUDE_OK)
        return 1;
    etude_assert(c, "broadcast-on-wire", end.end == ETUDE_END_EXITED && end.value == 0,
                 "pcapcheck found no broadcast datagram to 255.255.255.255 at port 7500");
    return 0;
}
