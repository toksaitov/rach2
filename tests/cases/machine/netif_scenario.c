/*
 * netif_scenario.c: the scenario of the netif and addressed cases
 * (../machine.netif.json and ../machine.addressed.json): netifprobe and
 * pingtest at the debug shell of a machine whose cards and boot line each
 * case chooses, their reports taken from the console, and what the kernel's
 * boot log and the card's capture say of the interfaces.
 *
 * The `probe` case runs netifprobe on a machine with QEMU's address. The
 * probe takes the interface down and brings it up again, and says so at
 * each; the scenario then sends a datagram from the host through the forward
 * of the case's port to the guest's port 9999, and the probe requires the
 * first dropped at the interface and the second received. The `unaddressed`
 * case boots with `rach2.ip=none`, and the probe configures the machine from
 * nothing.
 *
 * The other netif cases boot two cards, which are two interfaces, each with its
 * own name, station address and address (`twocards`, which also runs
 * `netifprobe second`). The driver table tries the RTL8139 first, so it is eth0
 * whichever slot it is in, and each card takes the address that `rach2.ifaddr`
 * gives its station address (`twomodels`). A card refused at its bring-up
 * leaves the other one serving (`refused`). pingtest shows in each that the
 * machine reaches the gateway; in `twomodels` the ping leaves by the second
 * interface, which is on the gateway's network.
 *
 * The `addressed` case gives the machine 10.0.2.77 on the boot line. That is
 * not QEMU's 10.0.2.15, which a kernel that ignored `rach2.ip=` would still
 * hold, nor pingtest's 10.0.2.99, which must stay unreachable. The capture
 * of the card, read once the machine has powered off, must have a packet
 * sent from 10.0.2.77, which only the IP layer can put there, and an ARP
 * request sent as 10.0.2.77, which shows that the address reached ARP.
 * Nothing on the network asks for the machine's address, so the case judges
 * what the machine asks as, not what it answers. It does not judge the
 * gateway the boot line gives: every destination in reach is on the subnet,
 * so nothing is routed, and the netif and dhcp cases judge routing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_report.h"
#include "netcase.h"

/* The probe's lines when it has taken the interface down and brought it up,
 * at each of which the host sends a datagram, and the start of its line
 * when a check failed. */
#define PROBE_DOWN "NETIFPROBE DOWN"
#define PROBE_UP   "NETIFPROBE UP"
#define PROBE_FAIL "NETIFPROBE FAIL: "

/* The address the `addressed` case gives the machine on its boot line. */
#define ADDRESSED "10.0.2.77"

/* A row of the boot log: its assertion, the text a line of the log has, and
 * the detail when no line has it. */
struct log_row {
    const char *id;
    const char *text;
    const char *why;
};

/** Type `command` at the debug shell, which has greeted, and give in
 * `*typed` the console offset after which the program it starts writes. */
static void start(struct net_boot *b, const char *command, uint64_t *typed)
{
    *typed = b->at;
    net_line(b, command);
}

/**
 * Make the report that `program`, started after console offset `typed`,
 * writes on the console the case's own, and move the console read past its
 * end; fail the attempt when the report is not whole or a wait for it did
 * not complete.
 */
static void take_report(struct net_boot *b, uint64_t typed, const char *program)
{
    uint64_t at;

    if (!net_ok(b->u))
        return;
    if (console_report(b->u->c, b->m, "guest", 1, typed, &at) != ETUDE_OK)
        net_fail(b->u, "%s: its report was not read whole", program);
    else
        b->at = at;
}

/** Run `command` at the debug shell, which has greeted, and take the report
 * of `program`, which it starts. */
static void run(struct net_boot *b, const char *command, const char *program)
{
    uint64_t typed;

    start(b, command, &typed);
    take_report(b, typed, program);
}

/**
 * Read the console line by line after the offset read so far until a line
 * has `mark` or the start of the probe's failure, and say which: 1 for the
 * mark, and 0 for a failure or a wait that did not complete.
 */
static int wait_mark(struct net_boot *b, const char *mark)
{
    uint64_t from;
    char *line;
    int seen;

    for (;;) {
        from = b->at;
        if (net_wait(b, "\n") != ETUDE_OK)
            return 0;
        line = net_text(b, from, b->at);
        seen = strstr(line, mark) != NULL ? 1 : strstr(line, PROBE_FAIL) != NULL ? 0 : -1;
        free(line);
        if (seen >= 0)
            return seen;
    }
}

/** Send `text` as one datagram from the host to the forward of the case's
 * port 0, which QEMU passes to the guest's port 9999. */
static void send_datagram(struct net_case *u, const char *text)
{
    char port[12], output[32];
    char *argv[5];
    struct net_run r;

    snprintf(port, sizeof(port), "%u", etude_port(u->c, 0));
    snprintf(output, sizeof(output), "hostpeer-%s.out", text);
    argv[0] = "hostpeer";
    argv[1] = "send";
    argv[2] = port;
    argv[3] = (char *)text;
    argv[4] = NULL;
    if (net_host(u, argv, output, &r) == ETUDE_OK && !net_exited0(&r))
        fprintf(stderr, "netif_scenario: hostpeer did not send \"%s\"\n", text);
    net_run_free(&r);
}

/** Report each of the `n` rows of `rows` against the console of `b` from
 * the boot's start to the offset read so far. */
static void log_rows(struct net_boot *b, const struct log_row *rows, size_t n)
{
    char *log = net_text(b, 0, b->at);
    size_t i;

    for (i = 0; i < n; i++)
        net_assert(b->u, rows[i].id, net_find(log, rows[i].text) != NULL, "%s", rows[i].why);
    free(log);
}

/** Give in `buf` of `size` bytes the rest of the first line of `log` after
 * `prefix`, the station address that follows it; an empty text when no line
 * has it. */
static const char *station(const char *log, const char *prefix, char *buf, size_t size)
{
    char line[256];
    const char *at;

    net_copy_line(net_find(log, prefix), line, sizeof(line));
    at = strstr(line, prefix);
    snprintf(buf, size, "%s", at != NULL ? at + strlen(prefix) : "");
    return buf;
}

/** Run pingtest at the debug shell, which has greeted, and take its
 * report. */
static void pingtest(struct net_case *u, struct net_boot *b)
{
    net_step(u, "pingtest");
    run(b, "exec /pingtest", "pingtest");
}

/** The `probe` cases: netifprobe, and a datagram from the host at each of
 * its marks that comes before a failure. */
static void probe_case(struct net_case *u, struct net_boot *b)
{
    uint64_t typed;

    net_step(u, "netifprobe");
    start(b, "exec /netifprobe", &typed);
    if (wait_mark(b, PROBE_DOWN)) {
        send_datagram(u, "down");
        if (wait_mark(b, PROBE_UP))
            send_datagram(u, "up");
    }
    take_report(b, typed, "netifprobe");
}

/** The `unaddressed` cases: netifprobe on a machine with no address. */
static void unaddressed_case(struct net_case *u, struct net_boot *b)
{
    net_step(u, "netifprobe");
    run(b, "exec /netifprobe", "netifprobe");
}

/** The `twocards` case: pingtest, `netifprobe second`, and the two cards'
 * lines of the boot log. */
static void twocards_case(struct net_case *u, struct net_boot *b)
{
    static const struct log_row cards[] = {
        {"eth0-e1000", "net: eth0: e1000", "the first card did not become eth0"},
        {"eth1-e1000", "net: eth1: e1000", "the second card did not become eth1"},
    };
    static const struct log_row addresses[] = {
        {"eth1-by-station", "net: eth1: configured by its station address",
         "the second card was not configured by its station address"},
        {"eth1-address", "net: eth1: IPv4 10.0.3.5/24, gateway 10.0.3.2",
         "the second card did not take the address it was given"},
        {"eth0-address", "net: eth0: IPv4 10.0.2.15/24", "the first card lost its own address"},
    };
    char first[64], second[64];
    char *log;

    pingtest(u, b);
    net_step(u, "netifprobe-second");
    net_wait(b, NET_SHELL);
    run(b, "exec /netifprobe second", "netifprobe");
    net_step(u, "log");
    log_rows(b, cards, sizeof(cards) / sizeof(cards[0]));
    log = net_text(b, 0, b->at);
    station(log, "net: eth0: e1000, station ", first, sizeof(first));
    station(log, "net: eth1: e1000, station ", second, sizeof(second));
    free(log);
    net_assert(u, "stations-differ", first[0] != '\0' && strcmp(first, second) != 0,
               "the cards report the station addresses \"%s\" and \"%s\"", first, second);
    log_rows(b, addresses, sizeof(addresses) / sizeof(addresses[0]));
}

/** The `twomodels` case: pingtest, and the lines of the RTL8139 and the
 * e1000 in the boot log. */
static void twomodels_case(struct net_case *u, struct net_boot *b)
{
    static const struct log_row rows[] = {
        {"rtl8139-eth0", "net: eth0: RTL8139", "the RTL8139 did not become eth0"},
        {"e1000-eth1", "net: eth1: e1000", "the e1000 did not become eth1"},
        {"rtl8139-address", "net: eth0: IPv4 10.0.3.5/24, gateway 10.0.3.2",
         "the RTL8139 did not take its own address"},
        {"e1000-address", "net: eth1: IPv4 10.0.2.15/24, gateway 10.0.2.2",
         "the e1000 did not take its own address"},
    };

    pingtest(u, b);
    net_step(u, "log");
    log_rows(b, rows, sizeof(rows) / sizeof(rows[0]));
}

/** The `refused` case: pingtest, and the refusal of the first card in the
 * boot log with one interface up. */
static void refused_case(struct net_case *u, struct net_boot *b)
{
    static const struct log_row rows[] = {
        {"refused", "net: eth0: refused: rach2.nicfail=0", "the first card was not refused"},
    };
    unsigned up;
    char *log;

    pingtest(u, b);
    net_step(u, "log");
    log_rows(b, rows, sizeof(rows) / sizeof(rows[0]));
    log = net_text(b, 0, b->at);
    up = net_count(log, "net: eth#: e1000, station");
    free(log);
    net_assert(u, "one-interface", up == 1, "%u interfaces came up, not one", up);
}

/** The `addressed` case: pingtest, the machine powered off, and its capture
 * read for what it sent as the address of its boot line. */
static void addressed_case(struct net_case *u, struct net_boot *b)
{
    char *check[] = {"pcapcheck", "bcast",      NULL,      "--source",
                     ADDRESSED,   "--arp-from", ADDRESSED, NULL};
    struct etude_termination end;
    const char *capture = NULL;
    struct net_run r;

    pingtest(u, b);
    net_step(u, "poweroff");
    net_wait(b, NET_SHELL);
    net_line(b, "poweroff");
    end.end = ETUDE_END_LOST;
    if (net_ok(u) && etude_wait_end(b->m, &end) != ETUDE_OK)
        net_fail(u, "the machine's end was not seen");
    net_assert(u, "powered-off", end.end == ETUDE_END_SHUTDOWN,
               "the machine did not power off at the debug shell's command: it ended as %d",
               (int)end.end);
    net_step(u, "capture");
    if (net_ok(u) && (capture = etude_capture(b->m, "net0")) == NULL)
        net_fail(u, "the card's frames were not recorded");
    check[2] = (char *)capture;
    net_host(u, check, "pcapcheck.out", &r);
    net_assert(u, "sent-as-address", net_exited0(&r),
               "pcapcheck found no packet sent from " ADDRESSED
               " or no ARP request sent as " ADDRESSED);
    net_run_free(&r);
}

/* A case of the scenario: its identity and its steps after the boot. */
struct netif_case {
    const char *id;
    void (*run)(struct net_case *u, struct net_boot *b);
};

/**
 * Boot the machine `guest` of the case the identity names to the debug
 * shell and run the case's steps; refuse an identity that is no case of
 * the two families.
 */
int etude_scenario(struct etude_case *c)
{
    static const struct netif_case cases[] = {
        {"machine.netif.probe", probe_case},       {"machine.netif.unaddressed", unaddressed_case},
        {"machine.netif.twocards", twocards_case}, {"machine.netif.twomodels", twomodels_case},
        {"machine.netif.refused", refused_case},   {"machine.addressed.pingtest", addressed_case},
    };
    const struct netif_case *k = NULL;
    struct net_case u;
    struct net_boot b;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(etude_case_id(c), cases[i].id) == 0)
            k = &cases[i];
    }
    if (k == NULL) {
        fprintf(stderr, "netif_scenario: %s is no case of machine.netif or machine.addressed\n",
                etude_case_id(c));
        return 1;
    }
    net_case(&u, c);
    net_step(&u, "boot");
    net_boot(&b, &u, "guest", 1, NULL);
    net_wait(&b, NET_SHELL);
    k->run(&u, &b);
    return net_end(&u);
}
