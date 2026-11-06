/*
 * raw_scenario.c: the scenario of the raw socket cases (../machine.raw.json):
 * rawprobe on each path of its own boot, and the two programs built on the raw
 * socket, traceroute and ping, on os-real.
 */
#include <stdio.h>
#include <string.h>

#include "console_report.h"

/* The debug shell's banner, after which it reads commands: once at boot, and
 * again each time a program it ran has exited. */
#define SHELL_READY "Type 'help' for available commands."

/**
 * Boot the test image, run rawprobe at the debug shell and take its report
 * from the console. With `capture`, power the machine off once the probe has
 * exited, so that the capture of its card is whole, and run `pcapcheck
 * rawwire` on it.
 */
static int rawprobe(struct etude_case *c, int capture)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    char *argv[4];
    uint64_t at;

    if (etude_step(c, "boot") != ETUDE_OK || etude_boot(c, "guest", NULL, &m, &end) != ETUDE_OK ||
        etude_console_wait(m, SHELL_READY, 0, &at) != ETUDE_OK)
        return 1;
    if (etude_step(c, "rawprobe") != ETUDE_OK || etude_type(m, "exec /rawprobe\r") != ETUDE_OK ||
        console_report(c, m, "guest", 1, at, &at) != ETUDE_OK)
        return 1;
    if (!capture)
        return 0;
    if (etude_step(c, "poweroff") != ETUDE_OK ||
        etude_console_wait(m, SHELL_READY, at, &at) != ETUDE_OK ||
        etude_type(m, "poweroff\r") != ETUDE_OK || etude_wait_end(m, &end) != ETUDE_OK)
        return 1;
    if (end.end != ETUDE_END_SHUTDOWN) {
        fprintf(stderr, "raw_scenario: the machine did not power off\n");
        return 1;
    }
    argv[0] = "pcapcheck";
    argv[1] = "rawwire";
    argv[2] = (char *)etude_capture(m, "net0");
    argv[3] = NULL;
    if (etude_step(c, "rawwire") != ETUDE_OK || argv[2] == NULL ||
        etude_host(c, argv, "rawwire", &end) != ETUDE_OK)
        return 1;
    etude_assert(c, "rawwire", end.end == ETUDE_END_EXITED && end.value == 0,
                 "pcapcheck rawwire failed; evidence rawwire.err says which send was wrong");
    return 0;
}

/**
 * Read the first line of artifact `name`, a password, into `buf` of `size`
 * bytes followed by a carriage return, as it is typed. Return 0, or -1 when
 * the artifact cannot be read.
 */
static int password(struct etude_case *c, const char *name, char *buf, size_t size)
{
    const char *path = etude_artifact(c, name);
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    size_t n;

    if (f == NULL || fgets(buf, (int)size - 1, f) == NULL) {
        fprintf(stderr, "raw_scenario: %s: the password cannot be read\n", name);
        if (f != NULL)
            fclose(f);
        return -1;
    }
    fclose(f);
    n = strcspn(buf, "\r\n");
    buf[n] = '\r';
    buf[n + 1] = '\0';
    return 0;
}

/** One console session of os-real: who logs in, what runs, what it shows. */
struct session {
    const char *user;    /* the account to log in as */
    const char *secret;  /* the artifact with the account's password */
    const char *prompt;  /* the shell's prompt once logged in */
    const char *step;    /* the step that runs `command` */
    const char *command; /* typed at the prompt, with its carriage return */
    const char *want;    /* the text the console must show after it */
    const char *id;      /* the assertion that `want` decides */
};

/**
 * Boot os-real and log in at the console as `s->user` with the password of
 * artifact `s->secret`, up to the shell's `s->prompt`, giving the machine in
 * `*m` and the console's offset after the prompt in `*at`. Returns 0, or 1
 * when the boot or the login did not get that far.
 */
static int login(struct etude_case *c, const struct session *s, struct etude_case_machine **m,
                 uint64_t *at)
{
    struct etude_termination end;
    char line[256];

    if (etude_step(c, "boot") != ETUDE_OK || etude_boot(c, "guest", NULL, m, &end) != ETUDE_OK ||
        etude_console_wait(*m, "rach2 login:", 0, at) != ETUDE_OK)
        return 1;
    snprintf(line, sizeof(line), "%s\r", s->user);
    if (etude_step(c, "login") != ETUDE_OK || etude_type(*m, line) != ETUDE_OK ||
        etude_console_wait(*m, "Password:", *at, at) != ETUDE_OK ||
        password(c, s->secret, line, sizeof(line)) != 0 || etude_type(*m, line) != ETUDE_OK ||
        etude_console_wait(*m, s->prompt, *at, at) != ETUDE_OK)
        return 1;
    return 0;
}

/**
 * Run `s->command` as step `s->step` at the console of `m` from offset
 * `*at`, and require `s->want` on the console after it as assertion `s->id`,
 * advancing `*at` past it. Returns 0 once the assertion is made, or 1 when
 * the case was stopped.
 */
static int run_row(struct etude_case *c, struct etude_case_machine *m, const struct session *s,
                   uint64_t *at)
{
    char line[256];
    enum etude_status st;

    if (etude_step(c, s->step) != ETUDE_OK || etude_type(m, s->command) != ETUDE_OK)
        return 1;
    st = etude_console_wait(m, s->want, *at, at);
    if (st == ETUDE_STOPPED)
        return 1;
    snprintf(line, sizeof(line), "the console never showed \"%s\"", s->want);
    etude_assert(c, s->id, st == ETUDE_OK, line);
    return 0;
}

/**
 * Boot os-real, log in at the console as `s->user` with the password of
 * artifact `s->secret`, run `s->command` at the shell's `s->prompt`, and
 * require `s->want` on the console after it as assertion `s->id`.
 */
static int session(struct etude_case *c, const struct session *s)
{
    struct etude_case_machine *m;
    uint64_t at;

    if (login(c, s, &m, &at) != 0)
        return 1;
    return run_row(c, m, s, &at);
}

/**
 * Log in as root and run traceroute toward 8.8.8.8, requiring its first hop
 * to be the gateway, which answers a hop limit of one: that is the whole
 * mechanism. The ping comes first because a datagram for an address whose
 * hardware address is not known yet is dropped while the request goes out,
 * and traceroute sends one probe per hop and does not retry, so a cold cache
 * costs it its first answer.
 */
static int traceroute(struct etude_case *c)
{
    static const struct session s = {
        "root",
        "root-password",
        "# ",
        "traceroute",
        "ping -c 1 10.0.2.2 > /dev/null; traceroute -n -q 1 -m 2 8.8.8.8\r",
        " 1  10.0.2.2",
        "first-hop"};

    return session(c, &s);
}

/**
 * Log in as the test user and ping the gateway with BusyBox's ping, which
 * for an ordinary account falls back from a raw socket, which is root's, to
 * an ICMP datagram socket. Its reply line has the hop limit the reply arrived
 * with, 255 from the emulator's gateway, which answers with the most there
 * is, read from IP_RECVTTL, since that socket gives no IPv4 header.
 */
static int userping(struct etude_case *c)
{
    static const struct session s = {"test", "test-password",        "$ ",
                                     "ping", "ping -c 1 10.0.2.2\r", "seq=0 ttl=255 time=",
                                     "reply"};

    return session(c, &s);
}

/**
 * Log in as the test user and ping the loopback address fifty times, 10 ms
 * apart, and then with no interval for a second. Each ping exits 0 only with
 * its replies counted: the first with all fifty, which -w makes it require,
 * and the second with one, which it reaches only when an interval of 0 keeps
 * the timer that sends the requests and checks -w running. The line the shell
 * prints when both exit 0 is computed, so that the echo of the command typed
 * cannot match it.
 */
static int fastping(struct etude_case *c)
{
    static const struct session s = {
        "test",
        "test-password",
        "$ ",
        "ping",
        "ping -q -i 0.01 -c 50 -w 10 127.0.0.1 && "
        "ping -q -i 0 -w 1 127.0.0.1 > /dev/null && echo fastping: $((6*7))\r",
        "fastping: 42",
        "all-replies"};

    return session(c, &s);
}

/**
 * Log in as the test user and ping the gateway with -I: by the card's name,
 * which must leave the first line as it is and get the reply, and by its
 * address, 10.0.2.15, which QEMU's user network leases, which must print the
 * source in its first line and get the reply; and by a name and an address the
 * machine does not have, each of which must print BusyBox's diagnostic and
 * exit 1. Each row prints its count of the lines it requires after its own
 * name, so that the echo of the command typed cannot match it.
 */
static int bindping(struct etude_case *c)
{
    static const struct session rows[] = {
        {"test", "test-password", "$ ", "name",
         "ping -c 1 -I eth0 10.0.2.2 | grep -c -e '^PING 10.0.2.2 (10.0.2.2): ' "
         "-e 'seq=0 ttl=' | sed 's/^/bind-name: /'\r",
         "bind-name: 2", "by-name"},
        {"test", "test-password", "$ ", "address",
         "ping -c 1 -I 10.0.2.15 10.0.2.2 | grep -c -e ' from 10.0.2.15: ' "
         "-e 'seq=0 ttl=' | sed 's/^/bind-address: /'\r",
         "bind-address: 2", "by-address"},
        {"test", "test-password", "$ ", "no-device",
         "{ ping -c 1 -I eth9 10.0.2.2 2>&1; echo status $?; } | grep -c -x "
         "-e \"ping: bad address 'eth9'\" -e 'status 1' | "
         "sed 's/^/bind-nodevice: /'\r",
         "bind-nodevice: 2", "no-device"},
        {"test", "test-password", "$ ", "no-address",
         "{ ping -c 1 -I 10.0.2.99 10.0.2.2 2>&1; echo status $?; } | grep -c -x "
         "-e 'ping: bind: Address not available' -e 'status 1' | "
         "sed 's/^/bind-noaddress: /'\r",
         "bind-noaddress: 2", "no-address"}};
    struct etude_case_machine *m;
    uint64_t at;
    size_t i;

    if (login(c, &rows[0], &m, &at) != 0)
        return 1;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (run_row(c, m, &rows[i], &at) != 0 ||
            etude_console_wait(m, rows[i].prompt, at, &at) == ETUDE_STOPPED)
            return 1;
    }
    return 0;
}

/**
 * Run the case that the last part of the case's identity names: rawprobe
 * with its capture checked, rawprobe on a boot with an address and no
 * gateway, root's traceroute, the test user's ping, the test user's fifty
 * pings of the loopback address, or the test user's pings with -I.
 */
int etude_scenario(struct etude_case *c)
{
    const char *which = strrchr(etude_case_id(c), '.');

    if (which != NULL && strcmp(which, ".rawprobe") == 0)
        return rawprobe(c, 1);
    if (which != NULL && strcmp(which, ".static") == 0)
        return rawprobe(c, 0);
    if (which != NULL && strcmp(which, ".traceroute") == 0)
        return traceroute(c);
    if (which != NULL && strcmp(which, ".userping") == 0)
        return userping(c);
    if (which != NULL && strcmp(which, ".fastping") == 0)
        return fastping(c);
    if (which != NULL && strcmp(which, ".bindping") == 0)
        return bindping(c);
    fprintf(stderr, "raw_scenario: %s is no case of machine.raw\n", etude_case_id(c));
    return 1;
}
