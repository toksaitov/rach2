/*
 * seabios_scenario.c: the scenario of tests/cases/machine.seabios.json, the
 * screen of a GRUB disk that SeaBIOS boots, on a framebuffer by default and
 * in the VGA's text mode from the menu's text entry.
 *
 * The first boot takes the default entry, on which GRUB sets the mode the
 * video BIOS prefers; the scenario reads the login prompt off a screen dump
 * with screentext and shows Doom's 320x200 picture scaled onto the whole
 * framebuffer, and the console drawing again once the game has gone. The
 * host then arms the text entry for one boot in the copy's GRUB environment
 * block, as grub-editenv would, and the second boot shows the console in 80
 * by 25 cells and Doom in the VGA's mode 13h. Each boot is driven at the
 * serial console as root, and every screen is judged by QMP's screen dump:
 * its size, and how many colors it has.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* What the console says when the boot is ready for a login. */
#define LOGIN "rach2 login:"

/* The kernel's record of the screen GRUB left it, from boot.c. */
#define FB_MODE   "boot: screen: framebuffer, rgb, "
#define TEXT_MODE "boot: screen: text, 80x25 cells"

/* The game's report of its first frame. */
#define FIRST_FRAME "doom: 1 frames in "

/* The header of GRUB's environment block, and the block's bytes. */
#define ENV_HEADER "# GRUB Environment Block"
#define ENV_BYTES  1024

/* The most colors a screen of the console has, and the fewest of a picture
 * of the game's. */
#define CONSOLE_COLORS 16
#define PICTURE_COLORS 64

/* The sizes in pixels of the VGA's text mode of 80 by 25 cells of 16 rows,
 * at 9 or 8 pixels a column, and of its mode 13h, whose 320x200 QEMU dumps
 * with each pixel doubled both ways. */
#define TEXT_HEIGHT 400
#define MODE13_W    640
#define MODE13_H    400

/* What the line that kills the game prints at its end, the first line the
 * console draws once the game has gone. */
#define GONE "DOOMGONE"

/* The lines typed at the root shell. Each prints a marker written with an
 * empty pair of quotes inside it, so that the line's echo is not taken for
 * it. */
#define WAD_LINE      "test -f /demo/doom1.wad && echo WAD\"\"HERE; echo WAD\"\"DONE"
#define START_LINE    "cd /root && (doom 2>&1 < /dev/null | tee /tmp/game.txt &)"
#define KILL_LINE     "killall doom; while pidof doom > /dev/null; do :; done; echo DOOM\"\"GONE"
#define CONSUMED_LINE "bootctl env next_entry; echo ENV\"\"DONE"

/* One attempt of the case. */
struct seabios {
    struct net_case u;
    char password[128];
};

/* A screen dump: its size, and the colors screentext counted, or -1. */
struct dump {
    unsigned long width, height;
    long colors;
};

/** Read the width and height of the binary PPM at `path` into `d`; 0 when
 * it is not one. */
static int dump_size(const char *path, struct dump *d)
{
    FILE *f = fopen(path, "rb");
    unsigned max = 0;
    int ok;

    d->width = d->height = 0;
    if (f == NULL)
        return 0;
    ok = fscanf(f, "P6 %lu %lu %u", &d->width, &d->height, &max) == 3 && max == 255;
    fclose(f);
    return ok;
}

/**
 * Dump the screen of boot `b` through QMP into evidence `name`, with its
 * size and colors in `d`; when `text` is not NULL, also run screentext on
 * it and give its run, the console's lines, in `text`, which the caller
 * frees with net_run_free.
 */
static void screen(struct seabios *s, struct net_boot *b, const char *name, struct dump *d,
                   struct net_run *text)
{
    char path[4096], args[4200], output[80];
    char *count[] = {"screentext", "--colors", path, NULL};
    char *read[] = {"screentext", path, NULL};
    struct net_run r;
    char *reply = NULL;

    memset(d, 0, sizeof(*d));
    d->colors = -1;
    if (text != NULL)
        memset(text, 0, sizeof(*text));
    if (net_path(&s->u, name, path, sizeof(path)) == NULL)
        return;
    snprintf(args, sizeof(args), "{\"filename\": \"%s\"}", path);
    if (net_ok(&s->u) && etude_qmp(b->m, "screendump", args, &reply) != ETUDE_OK)
        net_fail(&s->u, "QMP's screendump was not taken: %s", reply != NULL ? reply : "");
    free(reply);
    if (!net_ok(&s->u))
        return;
    if (!dump_size(path, d))
        net_fail(&s->u, "the screen dump %s is no binary PPM", name);
    snprintf(output, sizeof(output), "%s.colors", name);
    net_host(&s->u, count, output, &r);
    if (net_exited0(&r) && r.out != NULL && r.out[0] >= '0' && r.out[0] <= '9')
        d->colors = strtol(r.out, NULL, 10);
    net_run_free(&r);
    if (text != NULL) {
        snprintf(output, sizeof(output), "%s.txt", name);
        net_host(&s->u, read, output, text);
    }
}

/** Say whether a line of `text` begins with `start`. */
static int line_begins(const char *text, const char *start)
{
    const char *p;
    size_t n = strlen(start);

    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p != NULL ? p + 1 : NULL)
        if (strncmp(p, start, n) == 0)
            return 1;
    return 0;
}

/** Say whether screentext's run `r` read a line that begins with `start`. */
static int shows(const struct net_run *r, const char *start)
{
    return net_exited0(r) && line_begins(r->out, start);
}

/** Give the console of boot `b` from its start, from malloc. */
static char *console(struct net_boot *b)
{
    return net_file(b->console);
}

/**
 * Start the game in the background at the root shell of `b`, wait for its
 * first frame, and dump the screen into `name`; then kill the game, wait
 * until it has gone, and dump the screen again into `after`, with the
 * console's lines in `text` when it is not NULL.
 */
static void play(struct seabios *s, struct net_boot *b, const char *name, struct dump *game,
                 const char *after, struct dump *back, struct net_run *text)
{
    char *gone = NULL;

    memset(game, 0, sizeof(*game));
    memset(back, 0, sizeof(*back));
    game->colors = back->colors = -1;
    if (text != NULL)
        memset(text, 0, sizeof(*text));
    if (net_line(b, START_LINE) != ETUDE_OK || net_wait(b, FIRST_FRAME) != ETUDE_OK)
        return;
    screen(s, b, name, game, NULL);
    net_command(b, KILL_LINE, GONE, &gone);
    free(gone);
    screen(s, b, after, back, text);
}

/** End boot `b` with `poweroff -f` typed at its shell, and require the
 * machine's shutdown. */
static void power_off(struct net_boot *b)
{
    struct etude_termination end;

    if (net_line(b, "poweroff -f") != ETUDE_OK)
        return;
    if (etude_wait_end(b->m, &end) != ETUDE_OK || end.end != ETUDE_END_SHUTDOWN)
        net_fail(b->u, "poweroff -f did not shut the machine down");
}

/** End boot `b` with QMP's `quit` and wait for the machine to end. */
static void quit(struct net_boot *b)
{
    struct etude_termination end;
    char *reply = NULL;

    if (!net_ok(b->u))
        return;
    if (etude_qmp(b->m, "quit", NULL, &reply) != ETUDE_OK)
        net_fail(b->u, "QMP's quit was not taken");
    free(reply);
    if (net_ok(b->u) && etude_wait_end(b->m, &end) != ETUDE_OK)
        net_fail(b->u, "the machine did not end after QMP's quit");
}

/**
 * Give the offset of the first `ENV_HEADER` in the file at `path`, or -1
 * when it is not there or the file cannot be read.
 */
static long find_env(const char *path)
{
    FILE *f = fopen(path, "rb");
    static char buf[1 << 20];
    size_t want = strlen(ENV_HEADER), keep = 0, n;
    long base = 0, at = -1;
    char *p;

    if (f == NULL)
        return -1;
    while (at < 0 && (n = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
        n += keep;
        for (p = buf; p + want <= buf + n; p++) {
            if (memcmp(p, ENV_HEADER, want) == 0) {
                at = base + (p - buf);
                break;
            }
        }
        keep = n < want - 1 ? n : want - 1;
        memmove(buf, buf + n - keep, keep);
        base += (long)(n - keep);
    }
    fclose(f);
    return at;
}

/**
 * Arm the text entry for one boot in the copy of the disk: write the
 * environment block again as grub-editenv writes it, the header and its
 * warning, every other line kept, next_entry=text, and `#` to the block's
 * end. Report `armed`.
 */
static void arm_text(struct seabios *s)
{
    const char *copy = etude_private_copy(s->u.c, "seabios-disk");
    char block[ENV_BYTES + 1], out[ENV_BYTES + 1], *line, *next;
    long at = copy != NULL ? find_env(copy) : -1;
    size_t n, len;
    FILE *f = NULL;
    int ok = 0;

    if (at >= 0 && (f = fopen(copy, "r+b")) != NULL && fseek(f, at, SEEK_SET) == 0 &&
        fread(block, 1, ENV_BYTES, f) == ENV_BYTES) {
        block[ENV_BYTES] = '\0';
        n = (size_t)snprintf(out, sizeof(out),
                             "%s\n# WARNING: Do not edit this file by tools other than "
                             "grub-editenv!!!\n",
                             ENV_HEADER);
        for (line = block; line != NULL && *line != '\0'; line = next) {
            next = strchr(line, '\n');
            len = next != NULL ? (size_t)(next - line) : strlen(line);
            if (next != NULL)
                next++;
            while (len > 0 && line[len - 1] == '#')
                len--;
            if (len == 0 || line[0] == '#' || strncmp(line, "next_entry=", 11) == 0 ||
                n + len + 1 >= sizeof(out))
                continue;
            memcpy(out + n, line, len);
            n += len;
            out[n++] = '\n';
        }
        n += (size_t)snprintf(out + n, sizeof(out) - n, "next_entry=text\n");
        if (n < ENV_BYTES) {
            memset(out + n, '#', ENV_BYTES - n);
            ok = fseek(f, at, SEEK_SET) == 0 && fwrite(out, 1, ENV_BYTES, f) == ENV_BYTES;
        }
    }
    if (f != NULL)
        ok = fclose(f) == 0 && ok;
    net_assert(&s->u, "armed", ok,
               at < 0 ? "the copy of the disk has no GRUB environment block"
                      : "next_entry=text could not be written into the copy's environment block");
}

/**
 * Step `framebuffer`: the default entry on the mode the video BIOS
 * prefers, the login prompt read off the screen, Doom's picture scaled onto
 * the framebuffer and the console back once the game has gone.
 */
static void framebuffer(struct seabios *s)
{
    struct net_boot b;
    struct net_run text, after;
    struct dump shown, game, back;
    char *log = NULL, *wad = NULL, line[200] = "";
    const char *mode;
    unsigned long w = 0, h = 0;

    memset(&text, 0, sizeof(text));
    memset(&after, 0, sizeof(after));
    memset(&shown, 0, sizeof(shown));
    shown.colors = -1;
    game = back = shown;
    if (net_step(&s->u, "framebuffer") == ETUDE_OK &&
        net_boot(&b, &s->u, "seabios", 1, NULL) == ETUDE_OK &&
        net_login(&b, "root", s->password, "# ") == ETUDE_OK) {
        log = console(&b);
        net_command(&b, WAD_LINE, "WADDONE", &wad);
        screen(s, &b, "framebuffer.ppm", &shown, &text);
        play(s, &b, "doom.ppm", &game, "after.ppm", &back, &after);
        power_off(&b);
    }
    mode = log != NULL ? net_find(log, FB_MODE) : NULL;
    net_copy_line(mode, line, sizeof(line));
    if (mode != NULL)
        sscanf(strstr(line, FB_MODE) + strlen(FB_MODE), "%lux%lu", &w, &h);
    net_assert(&s->u, "fb-mode", w >= 640 && h >= 384,
               "the kernel did not name a framebuffer of at least 640x384 that GRUB left it: %s",
               line);
    net_assert(&s->u, "wad", net_find(wad, "WADHERE") != NULL,
               "the disk has no /demo/doom1.wad, which its root stages from ports/doom/doom1.wad"
               " (sh ports/doom/fetch_wad.sh)");
    net_assert(&s->u, "fb-screen", shown.width == w && shown.height == h,
               "the screen dump is %lux%lu, not the framebuffer's %lux%lu", shown.width,
               shown.height, w, h);
    net_assert(&s->u, "fb-login", shows(&text, LOGIN),
               "no line of the framebuffer console, as screentext reads it, begins %s", LOGIN);
    net_assert(&s->u, "fb-doom",
               game.width == w && game.height == h && game.colors > PICTURE_COLORS,
               "the screen was %lux%lu with %ld colors while the game ran, not the framebuffer's"
               " %lux%lu with more than %d",
               game.width, game.height, game.colors, w, h, PICTURE_COLORS);
    net_assert(&s->u, "fb-back",
               back.width == w && back.height == h && back.colors >= 0 &&
                   back.colors <= CONSOLE_COLORS && shows(&after, GONE),
               "once the game had gone the screen was %lux%lu with %ld colors, and %s line of"
               " it began %s",
               back.width, back.height, back.colors, shows(&after, GONE) ? "a" : "no", GONE);
    net_run_free(&text);
    net_run_free(&after);
    free(log);
    free(wad);
}

/**
 * Say whether the dump `d` is of the VGA's text mode of 80 by 25 cells, at
 * 9 or 8 pixels a column, with the console's colors.
 */
static int text_screen(const struct dump *d)
{
    return (d->width == 720 || d->width == 640) && d->height == TEXT_HEIGHT && d->colors > 1 &&
           d->colors <= CONSOLE_COLORS;
}

/**
 * Step `text`: the text entry armed from the host, the console in the
 * VGA's text mode, Doom in mode 13h, and the text mode back once the game
 * has gone.
 */
static void text(struct seabios *s)
{
    struct net_boot b;
    struct dump shown, game, back;
    char *log = NULL, *env = NULL, line[200] = "";

    memset(&shown, 0, sizeof(shown));
    shown.colors = -1;
    game = back = shown;
    net_step(&s->u, "text");
    arm_text(s);
    if (net_ok(&s->u) && net_boot(&b, &s->u, "seabios", 2, NULL) == ETUDE_OK &&
        net_login(&b, "root", s->password, "# ") == ETUDE_OK) {
        log = console(&b);
        net_command(&b, CONSUMED_LINE, "ENVDONE", &env);
        screen(s, &b, "text.ppm", &shown, NULL);
        play(s, &b, "mode13h.ppm", &game, "textback.ppm", &back, NULL);
        quit(&b);
    }
    net_copy_line(log != NULL ? net_find(log, "boot: screen: ") : NULL, line, sizeof(line));
    net_assert(&s->u, "text-mode", log != NULL && net_find(log, TEXT_MODE) != NULL,
               "the kernel did not say GRUB left it the text mode of 80x25 cells: %s", line);
    net_assert(&s->u, "text-consumed", net_find(env, "next_entry: empty") != NULL,
               "GRUB did not consume next_entry=text: bootctl env said %s",
               net_copy_line(net_find(env, "bootctl: next_entry"), line, sizeof(line)));
    net_assert(&s->u, "text-screen", text_screen(&shown),
               "the screen dump is %lux%lu with %ld colors, not the text mode's 720x400 or"
               " 640x400 with at most %d",
               shown.width, shown.height, shown.colors, CONSOLE_COLORS);
    net_assert(
        &s->u, "text-doom",
        game.width == MODE13_W && game.height == MODE13_H && game.colors > PICTURE_COLORS,
        "the screen was %lux%lu with %ld colors while the game ran, not mode 13h's doubled %dx%d"
        " with more than %d",
        game.width, game.height, game.colors, MODE13_W, MODE13_H, PICTURE_COLORS);
    net_assert(&s->u, "text-back", text_screen(&back),
               "once the game had gone the screen was %lux%lu with %ld colors, not the text"
               " mode's",
               back.width, back.height, back.colors);
    free(log);
    free(env);
}

int etude_scenario(struct etude_case *c)
{
    struct seabios s;

    memset(&s, 0, sizeof(s));
    if (strcmp(etude_case_id(c), "machine.seabios.screens") != 0)
        return 1;
    net_case(&s.u, c);
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    framebuffer(&s);
    text(&s);
    return net_end(&s.u);
}
