/*
 * hvc_scenario.c: the scenario of guest.hvc's machine cases. In `restart`
 * the debug shell restarts a machine whose log goes to a virtio console's
 * port beside COM1: the driver must give the console back to COM1 and reset
 * the device before the kernel restarts. In `panic` the kernel panics with
 * the virtio console's transmit lock held (`rach2.vconpanic`) on a machine
 * whose only console is the port, and the panic's record must reach the
 * host through it. In `ports` QMP adds a console port and a third port to
 * a virtio console of four ports after the boot, and removes the third,
 * while hvcmusl checks each port's nodes from the debug shell and the host
 * reads and writes the ports' ends, a file and an abstract Unix socket.
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lifecycle.h"

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Boot to the debug shell, type reboot, and wait for the console to leave
 * the port, the device's reset and the restart, in that order, and for the
 * machine's end, which must be a reset. Assertions `enabled`, `halted`,
 * `then-restart` and `reset`.
 */
static void restart(struct lc *l)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at = 0;

    if ((m = lc_shell(l, "restart", "fixture", &at)) == NULL)
        return;
    lc_find(l, m, "enabled", "console: enabled: hvc0, port 0 of virtio0, for output alone", 0, 1);
    if (!lc_type(l, m, "reboot\r") ||
        !lc_wait(l, m, "console: disabled: hvc0: the system stops", &at, NULL) ||
        !lc_wait(l, m, "virtio_console: shutdown: virtio0: reset", &at, "halted") ||
        !lc_wait(l, m, "reboot: restarting", &at, "then-restart") || !lc_end(l, m, &end))
        return;
    lc_assert(l, "reset", end.end == ETUDE_END_RESET, "QEMU's event is not a reset");
}

/**
 * Boot with the test's panic and wait for the port to carry the console and
 * then the panic's record; then end the machine, which the panic left
 * halted. The machine's console is the port, on which etude shows no
 * fault's end, so the panic's records are its declared critical text.
 * Assertions `enabled` and `panicked`.
 */
static void panic(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "fixture")) == NULL ||
        !lc_wait(l, m, "console: enabled: vport0p1, port 1 of ", &at, "enabled") ||
        !lc_wait(l, m, "panic: stopped: virtio_console: panic test: the transmit side is held", &at,
                 "panicked"))
        return;
    (void)lc_quit(l, m, NULL);
}

/* The most bytes of a port's host end the scenario reads. */
#define HOST_MAX 65536

/* The tries at a host end's file, each after a program's run at the debug
 * shell, before the bytes the guest wrote count as lost. */
#define HOST_TRIES 20

/** Copy the path of evidence `name` of the attempt into `out` of `size`
 * bytes; 0, or -1 with the scenario halted. */
static int evidence(struct lc *l, const char *name, char *out, size_t size)
{
    const char *path = etude_evidence(l->c, name);

    if (path == NULL || strlen(path) >= size) {
        lc_halt(l, ETUDE_FAILED, "no evidence path");
        return -1;
    }
    strcpy(out, path);
    return 0;
}

/** Send QMP `execute` with `args`; 1 when QEMU returned, else 0 with the
 * scenario halted. */
static int qmp(struct lc *l, struct etude_case_machine *m, const char *execute, const char *args)
{
    char *reply = NULL;
    enum etude_status st = etude_qmp(m, execute, args, &reply);

    if (st != ETUDE_OK) {
        fprintf(stderr, "hvc_scenario: %s %s: %s\n", execute, args, reply != NULL ? reply : "");
        lc_halt(l, st, execute);
    }
    free(reply);
    return st == ETUDE_OK;
}

/** Whether the file at `path`, a port's host end, holds `text`. */
static int host_has(const char *path, const char *text)
{
    static char buf[HOST_MAX + 1];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL)
        return 0;
    n = fread(buf, 1, HOST_MAX, f);
    fclose(f);
    buf[n] = '\0';
    return memmem(buf, n, text, strlen(text)) != NULL;
}

/** Wait, through runs of /bin/echo at the debug shell, which give the guest
 * the time its driver's thread needs, until the file at `path` holds
 * `text`; assertion `id` says whether it came. */
static void host_wait(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *path,
                      const char *text, const char *id)
{
    int i;

    for (i = 0; i < HOST_TRIES && !host_has(path, text); i++)
        free(lc_run(l, m, "fixture", "exec /bin/echo sync\r", at, NULL));
    lc_assert(l, id, host_has(path, text), text);
}

/** Connect to QEMU's listening host end of a port, the abstract Unix
 * socket `name`, and send it `text`; the descriptor, which reads without
 * waiting, or -1. */
static int sock_open(const char *name, const char *text)
{
    struct sockaddr_un a;
    size_t n = strlen(name);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, name, n);
    if (connect(fd, (struct sockaddr *)&a,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n)) != 0 ||
        write(fd, text, strlen(text)) != (ssize_t)strlen(text) ||
        fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** Wait, as host_wait does, until the socket `fd` has given `text`;
 * assertion `id` says whether it came. */
static void sock_wait(struct lc *l, struct etude_case_machine *m, uint64_t *at, int fd,
                      const char *text, const char *id)
{
    static char buf[HOST_MAX + 1];
    size_t got = 0;
    ssize_t n;
    int i;

    for (i = 0; i <= HOST_TRIES; i++) {
        while (got < HOST_MAX && (n = read(fd, buf + got, HOST_MAX - got)) > 0)
            got += (size_t)n;
        if (memmem(buf, got, text, strlen(text)) != NULL || i == HOST_TRIES)
            break;
        free(lc_run(l, m, "fixture", "exec /bin/echo sync\r", at, NULL));
    }
    lc_assert(l, id, memmem(buf, got, text, strlen(text)) != NULL, text);
}

/**
 * Boot to the debug shell on a machine whose boot line names hvc0 and then
 * ttyS0 and whose virtio console has the generic port org.rach2.boot, and
 * check that port's nodes; add a virtconsole, whose host's end must get
 * the driver's record of its addition, which only emerg_wr carries, since
 * the port takes the log only after it, and a line written to /dev/hvc0;
 * add org.rach2.late at port 3, whose host's end is a socket that the
 * scenario connects to and writes `pong` to, since QEMU closes a file's
 * end at the end of its input, and check it reads, writes and polls;
 * remove it and check its nodes go. Assertions `boot`, `emergency`,
 * `terminal`, `tty-out`, `late`, `late-out` and `removed`.
 */
static void ports(struct lc *l)
{
    struct etude_case_machine *m;
    char con_out[1024], late[64], args[3200];
    uint64_t at = 0;
    int fd;

    if (evidence(l, "hvc0.out", con_out, sizeof(con_out)) != 0)
        return;
    snprintf(late, sizeof(late), "rach2.hvc_scenario.%ld", (long)getpid());
    if ((m = lc_shell(l, "ports", "fixture", &at)) == NULL)
        return;
    free(lc_run(l, m, "fixture", "exec /musl/hvcmusl boot\r", &at, "boot"));

    snprintf(args, sizeof(args),
             "{\"id\": \"hvc0\", \"backend\": {\"type\": \"file\", \"data\": {\"out\": \"%s\"}}}",
             con_out);
    if (!qmp(l, m, "chardev-add", args) ||
        !qmp(l, m, "device_add",
             "{\"driver\": \"virtconsole\", \"bus\": \"vs.0\", \"chardev\": \"hvc0\", "
             "\"id\": \"con0\"}"))
        return;
    free(lc_run(l, m, "fixture", "exec /musl/hvcmusl tty\r", &at, "terminal"));
    host_wait(l, m, &at, con_out, "virtio_console: port: virtio0: port 0 added", "emergency");
    host_wait(l, m, &at, con_out, "hvc-tty\r\n", "tty-out");

    snprintf(args, sizeof(args),
             "{\"id\": \"late\", \"backend\": {\"type\": \"socket\", \"data\": {\"addr\": "
             "{\"type\": \"unix\", \"data\": {\"path\": \"%s\", \"abstract\": true}}, "
             "\"server\": true, \"wait\": false}}}",
             late);
    if (!qmp(l, m, "chardev-add", args))
        return;
    if ((fd = sock_open(late, "pong\n")) < 0) {
        lc_halt(l, ETUDE_FAILED, "the late port's host end does not connect");
        return;
    }
    if (qmp(l, m, "device_add",
            "{\"driver\": \"virtserialport\", \"bus\": \"vs.0\", \"nr\": 3, "
            "\"chardev\": \"late\", \"name\": \"org.rach2.late\", \"id\": \"late\"}")) {
        free(lc_run(l, m, "fixture", "exec /musl/hvcmusl late\r", &at, "late"));
        sock_wait(l, m, &at, fd, "ping\n", "late-out");
    }
    close(fd);
    if (!qmp(l, m, "device_del", "{\"id\": \"late\"}"))
        return;
    free(lc_run(l, m, "fixture", "exec /musl/hvcmusl gone\r", &at, "removed"));
    (void)lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const restart_ids[] = {"enabled", "halted", "then-restart", "reset", NULL};
    static const char *const panic_ids[] = {"enabled", "panicked", NULL};
    static const char *const ports_ids[] = {"boot", "emergency", "terminal", "tty-out",
                                            "late", "late-out",  "removed",  NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "restart") == 0) {
        lc_begin(&l, c, restart_ids);
        restart(&l);
    } else if (strcmp(name, "panic") == 0) {
        lc_begin(&l, c, panic_ids);
        panic(&l);
    } else if (strcmp(name, "ports") == 0) {
        lc_begin(&l, c, ports_ids);
        ports(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
