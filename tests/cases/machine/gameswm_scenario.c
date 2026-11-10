/*
 * gameswm_scenario.c: the scenarios of tests/cases/machine.games-wm.json,
 * fullscreen X and Wine windows, raw motion, Wine's sound and DirectInput in
 * the boot session's Weston on kiosk-shell, each row a boot of its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/** Boot machine `gnu` into `b`, log in as root, open the guest command
 * channel and mount the payload's disk, as step `boot`. */
static void booted(struct net_case *u, struct net_boot *b)
{
    char password[128];
    const char *argv[] = {"/bin/sh", "-c",
                          "mkdir -p /tmp/games && mount -t ext2 -o ro /dev/disk1 /tmp/games", NULL};
    struct net_run r;

    net_secret(u, "demo-password", password, sizeof(password));
    if (net_step(u, "boot") == ETUDE_OK)
        net_boot(b, u, "gnu", 1, NULL);
    net_seen(b, "ext2: root: /dev/disk0p1");
    net_seen(b, "rach2 login");
    if (net_login(b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b->m) != ETUDE_OK)
        net_fail(u, "the GNU guest command channel did not open");
    net_guest(u, b->m, argv, NULL, NULL, "mount", &r);
    if (net_ok(u) && !net_exited0(&r))
        net_fail(u, "the payload's disk did not mount");
    net_run_free(&r);
}

/** Send QMP's `execute` with `arguments`, failing the case on an error. */
static void qmp(struct net_case *u, struct net_boot *b, const char *execute, const char *arguments)
{
    char *reply = NULL;

    if (net_ok(u) && etude_qmp(b->m, execute, arguments, &reply) != ETUDE_OK)
        net_fail(u, "QMP %s %s: %s", execute, arguments, reply != NULL ? reply : "no reply");
    free(reply);
}

/** Press and release Escape, which ends each held window. */
static void escape(struct net_case *u, struct net_boot *b)
{
    qmp(u, b, "send-key", "{\"keys\": [{\"type\": \"qcode\", \"data\": \"esc\"}]}");
}

/** Move the pointer by DX, DY in one relative event of the PS/2 mouse. */
static void move(struct net_case *u, struct net_boot *b, int dx, int dy)
{
    char arguments[256];

    snprintf(arguments, sizeof(arguments),
             "{\"events\": [{\"type\": \"rel\", \"data\": {\"axis\": \"x\", \"value\": %d}}, "
             "{\"type\": \"rel\", \"data\": {\"axis\": \"y\", \"value\": %d}}]}",
             dx, dy);
    qmp(u, b, "input-send-event", arguments);
}

/**
 * Move the pointer by DX, DY and wait until wm.py's count of the kernel's
 * motion reaches X, Y, the sums of the moves so far. QEMU's PS/2 mouse keeps
 * what its queue of 16 bytes has no room for until its next event, so a move
 * sent before the guest has read the last could stay there.
 */
static void move_to(struct net_case *u, struct net_boot *b, int dx, int dy, int x, int y)
{
    char line[64];

    move(u, b, dx, dy);
    snprintf(line, sizeof(line), "wm: motion %d %d\n", x, y);
    if (net_ok(u) && etude_guest_output_wait(b->m, line) != ETUDE_OK)
        net_fail(u, "wm.py never printed \"wm: motion %d %d\"", x, y);
}

/**
 * Run wm.py's ROW through the guest channel and act on each of its ready
 * lines in turn: move the pointer once so that Weston's seat has one, move
 * it while xplace counts raw motion, and press Escape to end the window.
 * wm.py checks the screen itself, since QEMU's screendump has no surface
 * while virgl scans out a GL resource.
 */
static void row(struct net_case *u, struct net_boot *b, const char *row)
{
    static const char *const place[] = {"pointer", "xplace-1366", "full-1366", NULL};
    static const char *const scaled[] = {"xplace-800", "full-800", NULL};
    const char *const *readies = strcmp(row, "place") == 0    ? place
                                 : strcmp(row, "scaled") == 0 ? scaled
                                                              : NULL;
    const char *argv[] = {"/usr/bin/python3", "/tmp/games/wm.py", row, NULL};
    struct etude_guest_result result;
    struct net_run r;
    char ready[64], token[64];
    const char *path;
    int started = 0, i;

    memset(&r, 0, sizeof(r));
    if (net_step(u, "row") == ETUDE_OK) {
        started = etude_guest_start(b->m, argv, NULL, NULL) == ETUDE_OK;
        if (!started)
            net_fail(u, "wm.py did not start");
    }
    for (; readies != NULL && *readies != NULL && net_ok(u); readies++) {
        snprintf(ready, sizeof(ready), "wm: ready: %s", *readies);
        if (etude_guest_output_wait(b->m, ready) != ETUDE_OK) {
            net_fail(u, "wm.py never printed \"%s\"", ready);
            break;
        }
        if (strcmp(*readies, "pointer") == 0) {
            /* Weston makes the seat's pointer at the mouse's first event;
             * no window waits for a key yet. */
            move(u, b, 8, 8);
            continue;
        }
        if (strcmp(*readies, "xplace-1366") == 0) {
            /* wm.py's MOTION: -2000 on x, which pushes the pointer to the
             * window's left edge, and +200 on y. The i8042 passes the key's
             * bytes before the mouse's that wait, so the key follows the
             * kernel's count of the last move. */
            for (i = 1; i <= 10; i++)
                move_to(u, b, -200, 0, -200 * i, 0);
            for (i = 1; i <= 5; i++)
                move_to(u, b, 0, 40, -2000, 40 * i);
        }
        escape(u, b);
    }
    if (started && etude_guest_finish(b->m, !net_ok(u), "wm", &result) == ETUDE_OK) {
        path = etude_evidence(u->c, "wm");
        r.end = result.termination;
        r.out = net_file(path != NULL ? path : "");
    }
    snprintf(token, sizeof(token), "GAMESWM OK %s", row);
    net_assert(u, "row", net_ok(u) && r.out != NULL && net_exited0(&r) && strstr(r.out, token),
               "wm.py %s gave no \"%s\"; see wm and wm.err", row, token);
    net_run_free(&r);
}

/** Boot the GNU root on virgl and run the row the case's identity names. */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    const char *id = etude_case_id(c), *name = strrchr(id, '.');

    net_case(&u, c);
    memset(&b, 0, sizeof(b));
    b.u = &u;
    booted(&u, &b);
    row(&u, &b, name != NULL ? name + 1 : id);
    return net_end(&u);
}
