/*
 * raw_scenario.c: the scenario of the raw socket cases (../machine.raw.json
 * and ../machine.raw-e1000.json): rawprobe on each path of its own boot, and
 * the two programs built on the raw socket, traceroute and ping, on os-real.
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
 * Boot os-real, log in at the console as `s->user` with the password of
 * artifact `s->secret`, run `s->command` at the shell's `s->prompt`, and
 * require `s->want` on the console after it as assertion `s->id`.
 */
static int session(struct etude_case *c, const struct session *s)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    char line[256];
    enum etude_status st;
    uint64_t at;

    if (etude_step(c, "boot") != ETUDE_OK || etude_boot(c, "guest", NULL, &m, &end) != ETUDE_OK ||
        etude_console_wait(m, "rach2 login:", 0, &at) != ETUDE_OK)
        return 1;
    snprintf(line, sizeof(line), "%s\r", s->user);
    if (etude_step(c, "login") != ETUDE_OK || etude_type(m, line) != ETUDE_OK ||
        etude_console_wait(m, "Password:", at, &at) != ETUDE_OK ||
        password(c, s->secret, line, sizeof(line)) != 0 || etude_type(m, line) != ETUDE_OK ||
        etude_console_wait(m, s->prompt, at, &at) != ETUDE_OK)
        return 1;
    if (etude_step(c, s->step) != ETUDE_OK || etude_type(m, s->command) != ETUDE_OK)
        return 1;
    st = etude_console_wait(m, s->want, at, &at);
    if (st == ETUDE_STOPPED)
        return 1;
    snprintf(line, sizeof(line), "the console never showed \"%s\"", s->want);
    etude_assert(c, s->id, st == ETUDE_OK, line);
    return 0;
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
        "/bin/ping -c 1 10.0.2.2 > /dev/null; traceroute -n -q 1 -m 2 8.8.8.8\r",
        " 1  10.0.2.2",
        "first-hop"};

    return session(c, &s);
}

/**
 * Log in as the test user and ping the gateway with the in-house ping, the
 * one an ordinary account runs: busybox's needs a raw socket, which is
 * root's, and is not built for that reason. Its reply line has the hop limit
 * the reply arrived with, 255 from the emulator's gateway, which answers with
 * the most there is.
 */
static int userping(struct etude_case *c)
{
    static const struct session s = {"test",
                                     "test-password",
                                     "$ ",
                                     "ping",
                                     "/bin/ping -c 1 10.0.2.2\r",
                                     "icmp_seq=1 ttl=255 time=",
                                     "reply"};

    return session(c, &s);
}

/**
 * Run the case that the last part of the case's identity names: rawprobe
 * with its capture checked, rawprobe on a boot with an address and no
 * gateway, root's traceroute, or the test user's ping.
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
    fprintf(stderr, "raw_scenario: %s is no case of machine.raw\n", etude_case_id(c));
    return 1;
}
