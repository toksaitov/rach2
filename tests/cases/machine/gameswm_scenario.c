/*
 * gameswm_scenario.c: the scenarios of tests/cases/machine.games-wm.json,
 * fullscreen X and Wine windows, raw motion, Wine's sound and DirectInput in
 * the boot session's Weston on kiosk-shell, each row a boot of its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The output the row sets, and the points of the screen whose colours show
 * a window at its origin drawing its left half red and its right half
 * green across the whole output, scaled or not: each half's corners and the
 * two sides of the middle. */
#define SCREEN_W 1366
#define SCREEN_H 768
static const struct {
    int x, y, red;
} points[] = {
    {8, 8, 1}, {8, 759, 1}, {670, 384, 1}, {696, 384, 0}, {1357, 8, 0}, {1357, 759, 0},
};

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
 * Dump the screen into evidence `name` and report assertion `name`: a
 * SCREEN_W by SCREEN_H binary PPM whose sample points are red on the left
 * half and green on the right.
 */
static void screen(struct net_case *u, struct net_boot *b, const char *name)
{
    char path[4096], args[4200], magic[3] = "";
    unsigned long w = 0, h = 0;
    unsigned max = 0, i, bad = 0;
    unsigned char px[3];
    char *reply = NULL;
    FILE *f;

    if (net_path(u, name, path, sizeof(path)) == NULL)
        return;
    snprintf(args, sizeof(args), "{\"filename\": \"%s\"}", path);
    if (net_ok(u) && etude_qmp(b->m, "screendump", args, &reply) != ETUDE_OK)
        net_fail(u, "QMP's screendump was not taken: %s", reply != NULL ? reply : "");
    free(reply);
    if (!net_ok(u))
        return;
    f = fopen(path, "rb");
    if (f == NULL || fscanf(f, "%2s %lu %lu %u", magic, &w, &h, &max) != 4 ||
        strcmp(magic, "P6") != 0 || max != 255 || fgetc(f) == EOF) {
        net_assert(u, name, 0, "%s is no binary PPM", name);
        if (f != NULL)
            fclose(f);
        return;
    }
    long data = ftell(f);
    for (i = 0; w == SCREEN_W && h == SCREEN_H && i < sizeof(points) / sizeof(points[0]); i++) {
        if (fseek(f, data + ((long)points[i].y * (long)w + points[i].x) * 3, SEEK_SET) != 0 ||
            fread(px, 1, 3, f) != 3) {
            bad++;
            continue;
        }
        if (points[i].red ? !(px[0] > 200 && px[1] < 60 && px[2] < 60)
                          : !(px[0] < 60 && px[1] > 200 && px[2] < 60)) {
            printf("gameswm: %s: %d,%d is %d %d %d\n", name, points[i].x, points[i].y, px[0], px[1],
                   px[2]);
            bad++;
        }
    }
    fclose(f);
    net_assert(u, name, w == SCREEN_W && h == SCREEN_H && bad == 0,
               "%s: %lux%lu, %u points off the left-red, right-green halves", name, w, h, bad);
}

/**
 * Run wm.py's ROW through the guest channel and act on each of its ready
 * lines in turn: dump the screen while a window is held, move the pointer
 * while xplace counts raw motion, and press Escape to end the window.
 */
static void row(struct net_case *u, struct net_boot *b, const char *row)
{
    static const char *const place[] = {"xplace-1366", "full-1366", NULL};
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
        snprintf(token, sizeof(token), "screen-%s", *readies);
        screen(u, b, token);
        if (strcmp(*readies, "xplace-1366") == 0) {
            /* wm.py's MOTION: -2000 on x, which pushes the pointer to the
             * window's left edge, and +200 on y. */
            for (i = 0; i < 10; i++)
                move(u, b, -200, 0);
            for (i = 0; i < 5; i++)
                move(u, b, 0, 40);
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
