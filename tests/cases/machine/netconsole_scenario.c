/*
 * netconsole_scenario.c: the scenario of the netconsole cases,
 * tests/cases/machine.netconsole.json and machine.netconsole-e1000.json: the
 * kernel's log on the wire, on the card the case's machine has.
 *
 * A laptop with no serial port has no way to give its boot log but the
 * network, so the kernel sends its log records with `rach2.netconsole` as
 * broadcast frames of their own type. QEMU records every frame of the card
 * in a pcap file, which pcapcheck reads once the machine has ended, outside
 * the guest, where nothing in the guest can change it.
 *
 * The `boot` case requires a record the kernel printed before the card
 * existed, which only the buffer kept before the first send can carry, and
 * no text of the shell's: netconsole carries the log to whoever is on the
 * segment, and a person typing at a prompt did not agree to that. The
 * `stop` case types `poweroff` and requires the goodbye line on the wire and
 * nothing the kernel prints after it silences the card, which only a capture
 * can show: a line never sent and a line sent and missed look the same from
 * inside the machine.
 *
 * The `udp` case boots with a target of Linux's `netconsole=` in place of
 * the frames, types `fault` so that the kernel panics, and requires the log
 * from its first record to the panic's in datagrams to the target's port:
 * one record each, all there, in order and with checksums a receiver
 * accepts, which `nc -lu` on the host then reads.
 *
 * Each expected text has the `] ` that ends a record's prefix, so that what
 * pcapcheck finds is a whole record put together from the frames, not a
 * fragment of one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/** Run pcapcheck netconsole on `capture` with `args`, ended by NULL,
 * keeping its output as evidence `output`, and say whether it passed. */
static int pcapcheck(struct net_case *u, const char *capture, const char *output,
                     const char *const args[])
{
    char *argv[16] = {"pcapcheck", "netconsole", (char *)capture, "--min-frames", "4"};
    struct net_run r;
    size_t i;
    int ok;

    for (i = 0; args[i] != NULL && i + 6 < sizeof(argv) / sizeof(argv[0]); i++)
        argv[5 + i] = (char *)args[i];
    argv[5 + i] = NULL;
    net_host(u, argv, output, &r);
    ok = net_exited0(&r);
    net_run_free(&r);
    return ok;
}

/** Run pcapcheck netconsole-udp on `capture` for port 6666 with `args`, ended
 * by NULL, keeping its output as evidence `output`, and say whether it
 * passed. */
static int pcapcheck_udp(struct net_case *u, const char *capture, const char *output,
                         const char *const args[])
{
    char *argv[16] = {
        "pcapcheck", "netconsole-udp", (char *)capture, "--port", "6666", "--min-datagrams", "4"};
    struct net_run r;
    size_t i;
    int ok;

    for (i = 0; args[i] != NULL && i + 8 < sizeof(argv) / sizeof(argv[0]); i++)
        argv[7 + i] = (char *)args[i];
    argv[7 + i] = NULL;
    net_host(u, argv, output, &r);
    ok = net_exited0(&r);
    net_run_free(&r);
    return ok;
}

/** Give the capture of the boot's card, or NULL after failing the attempt. */
static const char *capture_of(struct net_boot *b)
{
    const char *path;

    if (!net_ok(b->u))
        return NULL;
    path = etude_capture(b->m, "net0");
    if (path == NULL)
        net_fail(b->u, "the card's frames were not recorded");
    return path;
}

/** The `boot` case: boot to the shell, wait for netconsole's record that
 * the records held while the link settled were sent, end the machine, and
 * read the frames. */
static void boot_case(struct net_case *u, struct net_boot *b)
{
    static const char *const contract[] = {NULL};
    static const char *const early[] = {"--expect", "] boot: Initializing system components", NULL};
    static const char *const ready[] = {"--expect", "] net: ready:", "--expect",
                                        "] netconsole: broadcast", NULL};
    static const char *const quiet[] = {"--reject", "Type 'help'", "--reject", "> ", NULL};
    struct etude_termination end;
    const char *capture;
    char *reply = NULL;

    net_assert(u, "shell", 1, "");
    net_step(u, "capture");
    net_seen(b, "] netconsole: settled:");
    if (net_ok(u) && etude_qmp(b->m, "quit", NULL, &reply) == ETUDE_FAILED)
        net_fail(u, "QEMU refused to quit");
    free(reply);
    if (net_ok(u) && etude_wait_end(b->m, &end) == ETUDE_LOST)
        net_fail(u, "the machine was lost before it ended");
    capture = capture_of(b);
    net_assert(u, "frames", pcapcheck(u, capture, "frames", contract),
               "the frames do not keep the header's contract");
    net_assert(u, "early-record", pcapcheck(u, capture, "early", early),
               "the record printed before the card existed is not on the wire");
    net_assert(u, "ready-records", pcapcheck(u, capture, "ready", ready),
               "the network's and netconsole's own records are not on the wire");
    net_assert(u, "no-console", pcapcheck(u, capture, "quiet", quiet),
               "the shell's greeting or prompt is on the wire");
}

/** The `stop` case: type `poweroff`, require the machine to power off, and
 * read the frames. */
static void stop_case(struct net_case *u, struct net_boot *b)
{
    static const char *const goodbye[] = {"--expect", "Stopping the system. Goodbye!", NULL};
    static const char *const silenced[] = {"--reject", "powering off", NULL};
    struct etude_termination end;
    const char *capture;

    end.end = ETUDE_END_LOST;
    net_assert(u, "shell", 1, "");
    net_step(u, "stop");
    net_line(b, "poweroff");
    net_wait(b, "Stopping the system");
    if (net_ok(u) && etude_wait_end(b->m, &end) == ETUDE_LOST)
        net_fail(u, "the machine was lost before it ended");
    net_assert(u, "powered-off", end.end == ETUDE_END_SHUTDOWN,
               "the machine did not power off: it ended as %d", (int)end.end);
    net_step(u, "capture");
    capture = capture_of(b);
    net_assert(u, "goodbye", pcapcheck(u, capture, "goodbye", goodbye),
               "the goodbye line is not on the wire");
    net_assert(u, "silenced", pcapcheck(u, capture, "silenced", silenced),
               "a line printed after the card was silenced is on the wire");
}

/** The `udp` case: panic the kernel from the shell, wait for the panic's
 * first report record, end the machine, and read the datagrams.
 *
 * A record reaches the serial port before netconsole, and a panic within the
 * link's settle sends every record held since the boot only once the panic's
 * own record reaches netconsole, so `panic: stopped` on the console comes
 * before any datagram. The panic writes its report records after that
 * delivery has sent them to the card, so the report's `build` line on the
 * console means the datagrams are in the capture. */
static void udp_case(struct net_case *u, struct net_boot *b)
{
    static const char *const contract[] = {NULL};
    static const char *const whole[] = {"--expect", "] boot: Initializing system components",
                                        "--expect", "] netconsole: started:",
                                        "--expect", "] panic: stopped:",
                                        NULL};
    static const char *const quiet[] = {"--reject", "Type 'help'", "--reject", "> ", NULL};
    struct etude_termination end;
    const char *capture;
    char *reply = NULL;

    net_assert(u, "shell", 1, "");
    net_step(u, "panic");
    net_line(b, "fault");
    net_wait(b, "panic: stopped");
    if (net_ok(u) && etude_fault(b->m, ETUDE_END_PANIC) != ETUDE_OK)
        net_fail(u, "the console has no record of the kernel's panic");
    net_wait(b, "] panic: report: build");
    net_step(u, "capture");
    if (net_ok(u) && etude_qmp(b->m, "quit", NULL, &reply) == ETUDE_FAILED)
        net_fail(u, "QEMU refused to quit");
    free(reply);
    if (net_ok(u) && etude_wait_end(b->m, &end) == ETUDE_LOST)
        net_fail(u, "the machine was lost before it ended");
    capture = capture_of(b);
    net_assert(u, "datagrams", pcapcheck_udp(u, capture, "datagrams", contract),
               "the datagrams are not whole, in order and checked");
    net_assert(u, "first-to-panic", pcapcheck_udp(u, capture, "whole", whole),
               "the log from the first record to the panic's is not in the datagrams");
    net_assert(u, "no-console", pcapcheck_udp(u, capture, "quiet", quiet),
               "the shell's greeting or prompt is in the datagrams");
}

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');
    struct net_case u;
    struct net_boot b;

    net_case(&u, c);
    net_step(&u, "boot");
    net_boot(&b, &u, "card", 1, NULL);
    net_wait(&b, NET_SHELL);
    if (dot != NULL && strcmp(dot, ".boot") == 0) {
        boot_case(&u, &b);
    } else if (dot != NULL && strcmp(dot, ".stop") == 0) {
        stop_case(&u, &b);
    } else if (dot != NULL && strcmp(dot, ".udp") == 0) {
        udp_case(&u, &b);
    } else {
        fprintf(stderr, "netconsole_scenario: no case %s\n", id);
        return 1;
    }
    return net_end(&u);
}
