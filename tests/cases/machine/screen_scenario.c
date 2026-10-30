/*
 * screen_scenario.c: the scenario of the screen cases (../machine.screen.json),
 * the console blanked and unblanked by TIOCLINUX, by the escape and by
 * consoleblank=, each judged by the kernel's records of its blanks and by
 * QMP's screen dumps, black while it is blank and showing the console
 * before and after.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* A press of Shift, which types nothing at the debug shell. */
static const char shift[] = "{\"keys\": [{\"type\": \"qcode\", \"data\": \"shift\"}]}";

/* The kernel's records of a blank and an unblank, the first of each. */
static const char blanked_on_request[] = "console: blanked: 1, on request";
static const char unblanked_on_request[] = "console: unblanked: 1, on request";
static const char blanked_idle[] = "console: blanked: 1, after the time without input";
static const char unblanked_key[] = "console: unblanked: 1, by a key or a mouse move";

/* The machine a case drives, and the offset its console has been read to. */
struct guest {
    struct lc *l;
    struct etude_case_machine *m;
    uint64_t at;
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Give the colors of the P6 picture at `path`: 0 when every pixel is black,
 * 0,0,0, 1 when every pixel is the first one's and that is not black, 2 when
 * another differs, and -1 when it is not a picture of 8-bit samples.
 */
static int picture_colors(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned w = 0, h = 0, max = 0;
    unsigned char first[3], px[3];
    unsigned long n, i;
    int colors = -1;

    if (f == NULL)
        return -1;
    if (fscanf(f, "P6 %u %u %u", &w, &h, &max) == 3 && max == 255 && fgetc(f) != EOF && w > 0 &&
        h > 0 && fread(first, 1, 3, f) == 3) {
        colors = 1;
        n = (unsigned long)w * h;
        for (i = 1; i < n && colors == 1; i++) {
            if (fread(px, 1, 3, f) != 3)
                colors = -1;
            else if (memcmp(px, first, 3) != 0)
                colors = 2;
        }
        if (colors == 1 && first[0] == 0 && first[1] == 0 && first[2] == 0)
            colors = 0;
    }
    fclose(f);
    return colors;
}

/**
 * Dump the screen into evidence `name` and report assertion `id`: it holds
 * when every pixel is black if `black` is set, and when the screen has more
 * than one color, the console's text, if it is clear.
 */
static void screen(struct guest *g, const char *name, int black, const char *id)
{
    char args[8300], detail[160], path[4096], *reply = NULL;
    const char *ev;
    enum etude_status st;
    size_t i, n = 0;
    int colors;

    if (!lc_ok(g->l))
        return;
    ev = etude_evidence(g->l->c, name);
    if (ev == NULL || strlen(ev) >= sizeof(path)) {
        lc_halt(g->l, ETUDE_FAILED, "the screen dump's path");
        return;
    }
    strcpy(path, ev);
    n = (size_t)snprintf(args, sizeof(args), "{\"filename\": \"");
    for (i = 0; path[i] != '\0' && n < sizeof(args) - 8; i++) {
        if (path[i] == '\\' || path[i] == '"')
            args[n++] = '\\';
        args[n++] = path[i];
    }
    snprintf(args + n, sizeof(args) - n, "\"}");
    st = etude_qmp(g->m, "screendump", args, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(g->l, st, "QMP's screendump");
        return;
    }
    colors = picture_colors(path);
    snprintf(detail, sizeof(detail), "the screen dump %s has %s, not %s", name,
             colors < 0    ? "no picture"
             : colors == 0 ? "only black"
             : colors == 1 ? "one color that is not black"
                           : "several colors",
             black ? "only black" : "the console's text");
    lc_assert(g->l, id, black ? colors == 0 : colors == 2, detail);
}

/** Press Shift on the machine's keyboard. */
static void press(struct guest *g)
{
    char *reply = NULL;
    enum etude_status st;

    if (!lc_ok(g->l))
        return;
    st = etude_qmp(g->m, "send-key", shift, &reply);
    free(reply);
    if (st != ETUDE_OK)
        lc_halt(g->l, st, "QMP's send-key");
}

/**
 * Run blankprobe with `args` at the debug shell, report assertion `id`
 * from its exit, and give its window, which the caller frees, or NULL;
 * `*from` gets where the run began.
 */
static char *probe(struct guest *g, const char *args, const char *id, uint64_t *from)
{
    char command[64];

    *from = g->at;
    snprintf(command, sizeof(command), "exec /blankprobe %s\n", args);
    return lc_run(g->l, g->m, "guest", command, &g->at, id);
}

/**
 * The case tioclinux: TIOCLINUX 14 blanks the console, a key leaves it
 * blank, and TIOCLINUX 4 shows it again.
 */
static void tioclinux_case(struct guest *g)
{
    uint64_t from;
    char *w;

    screen(g, "shown.ppm", 0, "shown");
    free(probe(g, "blank", "blank", &from));
    if (lc_ok(g->l))
        lc_wait(g->l, g->m, blanked_on_request, &from, "blanked");
    screen(g, "blank.ppm", 1, "black");
    press(g);
    if ((w = probe(g, "state", "state", &from)) != NULL) {
        lc_assert(g->l, "forced", strstr(w, "BLANKPROBE BLANKED 1") != NULL,
                  "TIOCLINUX 15 did not say the console stayed blank after a key");
        free(w);
    }
    free(probe(g, "unblank", "unblank", &from));
    if (lc_ok(g->l))
        lc_wait(g->l, g->m, unblanked_on_request, &from, "unblanked");
    screen(g, "back.ppm", 0, "back");
}

/**
 * The cases escape and bootline: the console blanks after the time without
 * input, set by the escape at one minute when `escape` is set and by the
 * boot line otherwise, and a key shows it again.
 */
static void idle_case(struct guest *g, int escape)
{
    uint64_t from;

    screen(g, "shown.ppm", 0, "shown");
    if (escape)
        free(probe(g, "time 1", "time", &from));
    if (lc_ok(g->l))
        lc_wait(g->l, g->m, blanked_idle, &g->at, "blanked");
    screen(g, "blank.ppm", 1, "black");
    press(g);
    if (lc_ok(g->l))
        lc_wait(g->l, g->m, unblanked_key, &g->at, "unblanked");
    screen(g, "back.ppm", 0, "back");
}

int etude_scenario(struct etude_case *c)
{
    static const char *const tioclinux_ids[] = {"default",   "shown", "blank",  "blanked",
                                                "black",     "state", "forced", "unblank",
                                                "unblanked", "back",  NULL};
    static const char *const escape_ids[] = {"never", "shown",     "time", "blanked",
                                             "black", "unblanked", "back", NULL};
    static const char *const bootline_ids[] = {"boot-time", "shown", "blanked", "black",
                                               "unblanked", "back",  NULL};
    const char *name = case_name(c);
    int tioclinux = strcmp(name, "tioclinux") == 0, escape = strcmp(name, "escape") == 0;
    const char *const *ids = tioclinux ? tioclinux_ids : escape ? escape_ids : bootline_ids;
    const char *told = tioclinux ? "console: blank: after 600 seconds without input"
                       : escape  ? "console: blank: never"
                                 : "console: blank: after 30 seconds without input";
    struct guest g;
    struct lc l;

    lc_begin(&l, c, ids);
    memset(&g, 0, sizeof(g));
    g.l = &l;
    if ((tioclinux || escape || strcmp(name, "bootline") == 0) && lc_step(&l, "boot") &&
        (g.m = lc_boot(&l, "guest")) != NULL && lc_wait(&l, g.m, told, &g.at, ids[0]) &&
        lc_wait(&l, g.m, "Type 'help'", &g.at, NULL) && lc_wait(&l, g.m, "\n> ", &g.at, NULL) &&
        lc_step(&l, "blank")) {
        if (tioclinux)
            tioclinux_case(&g);
        else
            idle_case(&g, escape);
        lc_quit(&l, g.m, NULL);
    }
    return lc_finish(&l);
}
