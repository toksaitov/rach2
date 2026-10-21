/*
 * default_scenario.c: the scenario of the machine cases of guest.default,
 * the probes that need what a guest case of etude-guest cannot give them:
 * the designated init's role, the console as their terminal or their
 * standard input, no HOME, or a clock reference on the host. Each case
 * boots the test image, runs its probe from the kernel's debug shell, as
 * that shell's transient init on the serial console, and judges the record
 * with which the kernel hands the shell back. The case `timing` judges the
 * kernel's record of its clock's period accounting at boot.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
 * Answer clockmusl's requests on the bound UDP socket `fd` until killed: each
 * `CLOCKREF N` gets back `CLOCKREF N MONO REAL`, the host's monotonic and
 * calendar clocks in nanoseconds, read as the request is answered.
 */
static void reference_serve(int fd)
{
    struct sockaddr_in peer;
    socklen_t size;
    struct timespec mono, real;
    char packet[160], tail;
    unsigned sequence;
    ssize_t n;

    for (;;) {
        size = sizeof(peer);
        n = recvfrom(fd, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&peer, &size);
        if (n <= 0)
            continue;
        packet[n] = '\0';
        if (sscanf(packet, "CLOCKREF %u %c", &sequence, &tail) != 1 ||
            clock_gettime(CLOCK_MONOTONIC, &mono) != 0 || clock_gettime(CLOCK_REALTIME, &real) != 0)
            continue;
        n = snprintf(packet, sizeof(packet), "CLOCKREF %u %lld %lld", sequence,
                     (long long)mono.tv_sec * 1000000000LL + mono.tv_nsec,
                     (long long)real.tv_sec * 1000000000LL + real.tv_nsec);
        sendto(fd, packet, (size_t)n, 0, (struct sockaddr *)&peer, size);
    }
}

/**
 * Start the clock reference: a child that answers on a UDP port of 127.0.0.1,
 * which the guest reaches at QEMU's host, 10.0.2.2. @return the child's pid,
 * with the port in `*port`, or -1.
 */
static pid_t reference_start(unsigned *port)
{
    struct sockaddr_in a;
    socklen_t size = sizeof(a);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    pid_t pid;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, size) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &size) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    pid = fork();
    if (pid == 0)
        reference_serve(fd);
    close(fd);
    return pid;
}

/** End the clock reference `pid` and reap it. */
static void reference_stop(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
}

/**
 * Say whether a line of the `len` bytes of `w` is clockmusl's result of its
 * comparison with the reference, `CLOCKMUSL REFERENCE OK`, whole.
 */
static int reference_said(const char *w, size_t len)
{
    static const char result[] = "CLOCKMUSL REFERENCE OK";
    size_t at, end, n = sizeof(result) - 1;

    for (at = 0; at + n <= len; at++) {
        if ((at != 0 && w[at - 1] != '\n') || memcmp(w + at, result, n) != 0)
            continue;
        end = at + n;
        if (end < len && w[end] == '\r')
            end++;
        if (end < len && w[end] == '\n')
            return 1;
    }
    return 0;
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

    if (clock && (ref = reference_start(&port)) < 0) {
        lc_halt(l, ETUDE_FAILED, "the clock reference's socket");
        return;
    }
    snprintf(command, sizeof(command), p->command, port);
    if ((m = lc_shell(l, "probe", "probe", &at)) != NULL) {
        w = lc_run(l, m, "probe", command, &at, "exit");
        if (w != NULL && clock)
            lc_assert(l, "reference", reference_said(w, strlen(w)),
                      "clockmusl gave no result of its comparison with the reference");
        free(w);
        lc_quit(l, m, NULL);
    }
    reference_stop(ref);
}

/**
 * Find the kernel's record `timing: tick N Hz, ...` at the start of a line of
 * the `len` bytes of `w`, and give its level: 6 when its clock's period
 * accounting passed its check on fixed readings, 3 when it did not; another
 * level's record does not count. @return the level of the first record of
 * level 6, else of level 3, or 0 when there is none.
 */
static int timing_level(const char *w, size_t len)
{
    size_t at, from;
    int level = 0;

    for (at = 0; at < len; at++) {
        if (at != 0 && w[at - 1] != '\n')
            continue;
        from = lc_past_prefix(w, len, at);
        if (from == at || from + 8 > len || memcmp(w + from, "timing: ", 8) != 0)
            continue;
        if (w[at + 1] == '6')
            return 6;
        if (w[at + 1] == '3')
            level = 3;
    }
    return level;
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
        level = timing_level(w, strlen(w));
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
