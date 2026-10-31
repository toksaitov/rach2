/*
 * termconsole_scenario.c: the scenario of tests/cases/machine.termconsole.json,
 * term as the console session of a disk whose menu carries
 * rach2.console=term, under OVMF and under SeaBIOS.
 *
 * COM1 shows the kernel console's output and nothing of term's screen, so
 * the scenario reads term through QMP's screen dumps: screentext --term
 * reads its grid, and libtsm's default foreground, which is none of the
 * kernel console's colors, tells its screen from the console's. The login
 * and the lines after it are typed with QMP's send-key, since COM1's bytes
 * do not reach the console while term holds the keyboard in K_MEDIUMRAW;
 * each line writes what it found to /dev/console, which COM1 shows, with
 * its own CR and LF, since term leaves the console in raw mode. Once term
 * is killed the kernel console is back, and its login is taken on COM1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The kernel's record of the framebuffer GRUB left it, from boot.c. */
#define FB_MODE "boot: screen: framebuffer, rgb, "

/* What init says on COM1 as it starts term, and once term is killed. */
#define STARTING "init: console: starting term"
#define KILLED   "init: console: term: killed by signal 9; login on the kernel console"

/* The prompts, as a line of a screen's grid begins with them. */
#define LOGIN    "rach2 login:"
#define PASSWORD "Password:"

/* libtsm's default foreground, which term draws text in. The kernel
 * console's sixteen colors are the VGA's, whose grey is 170,170,170. */
#define TERM_FG_R 229
#define TERM_FG_G 229
#define TERM_FG_B 229

/* The fewest colors of a picture of the game's, and the most counted. */
#define PICTURE_COLORS 64

/* The most screen dumps one wait for the screen takes before it fails. A
 * dump and its reading take tens of milliseconds, so this bounds a wait at
 * minutes of the emulator's time, longer than any of these needs. */
#define DUMPS_MAX 4000

/*
 * The line that probes term from inside it: in raw mode, without echo, it
 * sends XTVERSION, DECRQM for mode 2026, the Kitty keyboard query and DA1,
 * reads the replies up to DA1's final `c`, the only `c` in any of them, and
 * writes them to /dev/console between markers, ESC written as E.
 */
#define PROBE_LINE                                                                    \
    "stty raw -echo; printf '\\033[>q\\033[?2026$p\\033[?u\\033[c'; read -r -d c R; " \
    "stty sane; printf 'TERM''PROBE %sc PROBE''END\\r\\n' \"$R\" | tr '\\033' E > /dev/console"
#define PROBE_START "TERMPROBE "
#define PROBE_END   " PROBEEND"

/*
 * The line that runs Doom inside term: the game in the background, with no
 * terminal as its input, so that it takes the screen and leaves the keyboard;
 * a mark on COM1 at its first frame; then one byte read from /dev/console,
 * the key the scenario presses once it has dumped the screen, since term,
 * suspended, reads no key; then the game killed and waited for.
 */
#define DOOM_LINE                                                                         \
    "doom < /dev/null > /tmp/game.txt 2>&1 & until grep -q '1 frames in' /tmp/game.txt; " \
    "do sleep 1; done; printf 'DOOM''FRAME\\r\\n' > /dev/console; "                       \
    "dd if=/dev/console bs=1 count=1 > /dev/null 2>&1; kill $!; wait; "                   \
    "printf 'DOOM''GONE\\r\\n' > /dev/console"
#define DOOM_FRAME "DOOMFRAME"
#define DOOM_GONE  "DOOMGONE"

#define KILL_LINE "killall -KILL term"

/* One attempt of a case. */
struct tc {
    struct net_case u;
    char password[128];
    unsigned long width, height; /* the framebuffer the kernel named */
};

/* A screen dump: its size, its colors up to PICTURE_COLORS + 1, and whether
 * it has term's foreground. */
struct dump {
    unsigned long width, height;
    unsigned colors;
    int term_fg;
};

/** Judge the binary PPM at `path` into `d`; 0 when it is not one. */
static int judge(const char *path, struct dump *d)
{
    static unsigned long seen[PICTURE_COLORS + 1];
    unsigned char px[3];
    unsigned long n, i, c;
    unsigned max = 0, k;
    FILE *f = fopen(path, "rb");
    int ok = 0;

    memset(d, 0, sizeof(*d));
    if (f == NULL)
        return 0;
    if (fscanf(f, "P6 %lu %lu %u", &d->width, &d->height, &max) == 3 && max == 255 &&
        fgetc(f) != EOF) {
        ok = 1;
        n = d->width * d->height;
        for (i = 0; i < n; i++) {
            if (fread(px, 1, 3, f) != 3) {
                ok = 0;
                break;
            }
            if (px[0] == TERM_FG_R && px[1] == TERM_FG_G && px[2] == TERM_FG_B)
                d->term_fg = 1;
            c = (unsigned long)px[0] << 16 | (unsigned long)px[1] << 8 | px[2];
            for (k = 0; k < d->colors && seen[k] != c; k++) {
            }
            if (k == d->colors && d->colors <= PICTURE_COLORS)
                seen[d->colors++] = c;
        }
    }
    fclose(f);
    return ok;
}

/**
 * Dump the screen of boot `b` into the file at `path`, judge it into `d`,
 * and, when `grid` is not NULL, read its text with screentext and those
 * `grid` arguments, `--term 1` for term's grid or "" for the console's, into
 * `text`, which the caller frees with net_run_free.
 */
static void screen(struct tc *s, struct net_boot *b, const char *path, const char *name,
                   struct dump *d, const char *grid, struct net_run *text)
{
    char args[4200], output[80];
    char *term[] = {"screentext", "--term", "1", (char *)path, NULL};
    char *console[] = {"screentext", (char *)path, NULL};
    char *reply = NULL;

    memset(d, 0, sizeof(*d));
    if (text != NULL)
        memset(text, 0, sizeof(*text));
    if (!net_ok(&s->u))
        return;
    snprintf(args, sizeof(args), "{\"filename\": \"%s\"}", path);
    if (etude_qmp(b->m, "screendump", args, &reply) != ETUDE_OK)
        net_fail(&s->u, "QMP's screendump was not taken: %s", reply != NULL ? reply : "");
    free(reply);
    if (!net_ok(&s->u))
        return;
    if (!judge(path, d))
        net_fail(&s->u, "the screen dump %s is no binary PPM", name);
    if (grid != NULL && net_ok(&s->u)) {
        snprintf(output, sizeof(output), "%s.txt", name);
        net_host(&s->u, grid[0] != '\0' ? term : console, output, text);
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

/** Say whether a line of `text` ends with `c`. */
static int line_ends(const char *text, char c)
{
    const char *p;

    for (p = text; p != NULL && *p != '\0'; p++)
        if (*p == c && (p[1] == '\n' || p[1] == '\0'))
            return 1;
    return 0;
}

/* What a wait for the screen waits for. */
enum shown { TERM_LOGIN, TERM_PASSWORD, TERM_SHELL, TERM_BACK, CONSOLE_LOGIN };

/** Say whether dump `d`, whose grid read `text`, shows `want`. */
static int shows(const struct tc *s, enum shown want, const struct dump *d,
                 const struct net_run *text)
{
    const char *t = net_exited0(text) ? text->out : NULL;

    if (d->width != s->width || d->height != s->height)
        return 0;
    switch (want) {
    case TERM_LOGIN:
        return d->term_fg && line_begins(t, LOGIN);
    case TERM_PASSWORD:
        return d->term_fg && line_begins(t, PASSWORD);
    case TERM_SHELL:
        return d->term_fg && line_ends(t, '#');
    case TERM_BACK:
        return d->term_fg && d->colors <= PICTURE_COLORS;
    default:
        return !d->term_fg && line_begins(t, LOGIN);
    }
}

/**
 * Dump the screen of `b` into evidence `name` until it shows `want`, at most
 * DUMPS_MAX times, and leave the last dump's judgment in `d`. Returns whether
 * it showed.
 */
static int await(struct tc *s, struct net_boot *b, const char *name, enum shown want,
                 struct dump *d)
{
    char path[4096];
    struct net_run text;
    const char *grid = want == TERM_BACK ? NULL : want == CONSOLE_LOGIN ? "" : "--term";
    unsigned i;
    int seen = 0;

    memset(d, 0, sizeof(*d));
    if (net_path(&s->u, name, path, sizeof(path)) == NULL)
        return 0;
    for (i = 0; i < DUMPS_MAX && !seen && net_ok(&s->u); i++) {
        screen(s, b, path, name, d, grid, &text);
        seen = net_ok(&s->u) && shows(s, want, d, &text);
        net_run_free(&text);
    }
    return seen;
}

/** Give the qcode of the key that types `c` into `key`, with `shift` set
 * when Shift is held for it; 0 for a character no key types. */
static int qcode(char c, char *key, size_t size, int *shift)
{
    static const char plain[] = "-=[];'`\\,./ \r";
    static const char *const plain_names[] = {
        "minus",      "equal",        "bracket_left", "bracket_right", "semicolon",
        "apostrophe", "grave_accent", "backslash",    "comma",         "dot",
        "slash",      "spc",          "ret"};
    static const char shifted[] = "_+{}:\"~|<>?!@#$%^&*()";
    static const char *const shifted_names[] = {"minus",
                                                "equal",
                                                "bracket_left",
                                                "bracket_right",
                                                "semicolon",
                                                "apostrophe",
                                                "grave_accent",
                                                "backslash",
                                                "comma",
                                                "dot",
                                                "slash",
                                                "1",
                                                "2",
                                                "3",
                                                "4",
                                                "5",
                                                "6",
                                                "7",
                                                "8",
                                                "9",
                                                "0"};
    const char *p;

    *shift = 0;
    if (c >= 'a' && c <= 'z') {
        snprintf(key, size, "%c", c);
    } else if (c >= 'A' && c <= 'Z') {
        snprintf(key, size, "%c", c - 'A' + 'a');
        *shift = 1;
    } else if (c >= '0' && c <= '9') {
        snprintf(key, size, "%c", c);
    } else if (c != '\0' && (p = strchr(plain, c)) != NULL) {
        snprintf(key, size, "%s", plain_names[p - plain]);
    } else if (c != '\0' && (p = strchr(shifted, c)) != NULL) {
        snprintf(key, size, "%s", shifted_names[p - shifted]);
        *shift = 1;
    } else {
        return 0;
    }
    return 1;
}

/** Type `text` at boot `b` with QMP's send-key, a key, or Shift and a key,
 * for each character; QEMU queues the presses in turn. */
static void type(struct tc *s, struct net_boot *b, const char *text)
{
    char args[256], key[32];
    char *reply = NULL;
    int shift;

    for (; *text != '\0' && net_ok(&s->u); text++) {
        if (!qcode(*text, key, sizeof(key), &shift)) {
            net_fail(&s->u, "no key types the character %d", *text);
            return;
        }
        snprintf(args, sizeof(args), "{\"keys\": [%s{\"type\": \"qcode\", \"data\": \"%s\"}]}",
                 shift ? "{\"type\": \"qcode\", \"data\": \"shift\"}, " : "", key);
        if (etude_qmp(b->m, "send-key", args, &reply) != ETUDE_OK)
            net_fail(&s->u, "QMP's send-key %s was not taken: %s", key, reply != NULL ? reply : "");
        free(reply);
        reply = NULL;
    }
}

/**
 * Step `term`: the boot, term's screen with its login prompt, and the login
 * typed at it. Reports `framebuffer`, `term-screen` and `term-login`.
 */
static void term(struct tc *s, struct net_boot *b, const char *machine)
{
    struct dump d;
    char *log = NULL, line[200] = "", typed[160];
    const char *mode;
    int prompt = 0, shell = 0;

    memset(&d, 0, sizeof(d));
    if (net_step(&s->u, "term") == ETUDE_OK && net_boot(b, &s->u, machine, 1, NULL) == ETUDE_OK &&
        net_wait(b, STARTING) == ETUDE_OK) {
        log = net_file(b->console);
        mode = log != NULL ? net_find(log, FB_MODE) : NULL;
        net_copy_line(mode, line, sizeof(line));
        if (mode != NULL)
            sscanf(strstr(line, FB_MODE) + strlen(FB_MODE), "%lux%lu", &s->width, &s->height);
        prompt = await(s, b, "login.ppm", TERM_LOGIN, &d);
    }
    net_assert(&s->u, "framebuffer", s->width >= 640 && s->height >= 384,
               "the kernel did not name a framebuffer of at least 640x384: %s", line);
    net_assert(&s->u, "term-screen", prompt,
               "the screen was %lux%lu, %s libtsm's foreground, and term's grid had no line that"
               " began %s",
               d.width, d.height, d.term_fg ? "with" : "without", LOGIN);
    type(s, b, "root\r");
    if (await(s, b, "password.ppm", TERM_PASSWORD, &d)) {
        snprintf(typed, sizeof(typed), "%s\r", s->password);
        type(s, b, typed);
        shell = await(s, b, "shell.ppm", TERM_SHELL, &d);
    }
    net_assert(&s->u, "term-login", shell,
               "the login typed by keys did not reach the root shell's # on term's grid");
    free(log);
}

/** Say whether `text` has `E[?`, digits and semicolons, and `final`, or
 * for `final` '$', `E[?2026;`, a digit and `$y`. */
static int reply(const char *text, char final)
{
    const char *p = text;

    while (p != NULL && (p = strstr(p, "E[?")) != NULL) {
        p += 3;
        if (final == '$') {
            if (strncmp(p, "2026;", 5) == 0 && p[5] >= '0' && p[5] <= '9' &&
                strncmp(p + 6, "$y", 2) == 0)
                return 1;
            continue;
        }
        while ((*p >= '0' && *p <= '9') || *p == ';')
            p++;
        if (*p == final)
            return 1;
    }
    return 0;
}

/** Step `probes`: term's replies to the four probes, read on COM1. */
static void probes(struct tc *s, struct net_boot *b)
{
    char *log = NULL, found[400] = "";
    const char *at;

    if (net_step(&s->u, "probes") == ETUDE_OK) {
        type(s, b, PROBE_LINE "\r");
        if (net_wait(b, PROBE_END) == ETUDE_OK)
            log = net_file(b->console);
    }
    at = log != NULL ? net_find(log, PROBE_START) : NULL;
    net_copy_line(at, found, sizeof(found));
    net_assert(&s->u, "xtversion", strstr(found, "EP>|libtsm(") != NULL,
               "no XTVERSION reply in: %s", found);
    net_assert(&s->u, "decrqm", reply(found, '$'), "no DECRQM reply for mode 2026 in: %s", found);
    net_assert(&s->u, "kitty", reply(found, 'u'), "no reply to the Kitty query in: %s", found);
    net_assert(&s->u, "da1", strstr(found, "E[?62;22c" PROBE_END) != NULL,
               "no DA1 reply, last, in: %s", found);
    free(log);
}

/** Step `doom`: the game inside term takes the screen and gives it back. */
static void doom(struct tc *s, struct net_boot *b)
{
    struct dump game, back;
    char path[4096];
    int frame = 0, again = 0;

    memset(&game, 0, sizeof(game));
    memset(&back, 0, sizeof(back));
    if (net_step(&s->u, "doom") == ETUDE_OK) {
        type(s, b, DOOM_LINE "\r");
        frame = net_wait(b, DOOM_FRAME) == ETUDE_OK;
        if (frame && net_path(&s->u, "doom.ppm", path, sizeof(path)) != NULL)
            screen(s, b, path, "doom.ppm", &game, NULL, NULL);
        type(s, b, " ");
        if (net_wait(b, DOOM_GONE) == ETUDE_OK)
            again = await(s, b, "back.ppm", TERM_BACK, &back);
    }
    net_assert(&s->u, "wad", frame,
               "the game reported no first frame; the disk needs /demo/doom1.wad, which its root"
               " stages from ports/doom/doom1.wad (sh ports/doom/fetch_wad.sh)");
    net_assert(&s->u, "doom",
               game.width == s->width && game.height == s->height && game.colors > PICTURE_COLORS,
               "the screen was %lux%lu with %u colors while the game ran, not the framebuffer's"
               " %lux%lu with more than %d",
               game.width, game.height, game.colors, s->width, s->height, PICTURE_COLORS);
    net_assert(&s->u, "doom-back", again,
               "once the game had gone the screen was %lux%lu with %u colors, %s libtsm's"
               " foreground",
               back.width, back.height, back.colors, back.term_fg ? "with" : "without");
}

/**
 * Step `fallback`: term killed, init's report, the kernel console's login
 * on the screen and on COM1, and the machine powered off.
 */
static void fallback(struct tc *s, struct net_boot *b)
{
    struct etude_termination end;
    struct dump d;
    int killed = 0, shown = 0, in = 0;

    memset(&d, 0, sizeof(d));
    if (net_step(&s->u, "fallback") == ETUDE_OK) {
        type(s, b, KILL_LINE "\r");
        killed = net_wait(b, KILLED) == ETUDE_OK;
    }
    net_assert(&s->u, "killed", killed, "init did not say: %s", KILLED);
    shown = await(s, b, "console.ppm", CONSOLE_LOGIN, &d);
    in = shown && net_login(b, "root", s->password, "# ") == ETUDE_OK;
    net_assert(&s->u, "console-login", in, "the screen was %lux%lu, %s libtsm's foreground, %s",
               d.width, d.height, d.term_fg ? "with" : "without",
               shown ? "and root did not log in on COM1"
                     : "and the console's grid had no line that began " LOGIN);
    if (in && net_line(b, "poweroff -f") == ETUDE_OK &&
        (etude_wait_end(b->m, &end) != ETUDE_OK || end.end != ETUDE_END_SHUTDOWN))
        net_fail(&s->u, "poweroff -f did not shut the machine down");
}

int etude_scenario(struct etude_case *c)
{
    struct tc s;
    struct net_boot b;
    const char *id = etude_case_id(c);
    int uefi = strcmp(id, "machine.termconsole.uefi") == 0;

    memset(&s, 0, sizeof(s));
    memset(&b, 0, sizeof(b));
    if (!uefi && strcmp(id, "machine.termconsole.bios") != 0)
        return 1;
    net_case(&s.u, c);
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    term(&s, &b, uefi ? "uefi" : "bios");
    probes(&s, &b);
    if (uefi)
        doom(&s, &b);
    fallback(&s, &b);
    return net_end(&s.u);
}
