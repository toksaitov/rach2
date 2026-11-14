/*
 * desktop_scenario.c: the scenarios of tests/cases/machine.desktop.json, the
 * desktop session at boot, headless or on a display, each case a boot of its
 * own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The guest paths of the display probe and the programs it runs. */
#define PROBE      "/tmp/desktop_display.py"
#define DRMCAPTURE "/tmp/drmcapture"
#define EVPROBE    "/tmp/evprobe"

/* The keys evprobe reads first, as the monitor's sendkey names them, and
 * the twelve that overflow the queue of the reader that reads nothing. */
static const char *const ev_keys[] = {"a", "volumeup", "volumedown", "audiomute"};
static const char *const ev_flood[] = {"b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m"};

/* What a browser's page must record: a left click, then these keys. */
static const char *const page_keys[] = {"r", "a", "c", "h", "2"};

/* The word the console's shell receives from the keyboard after Weston's
 * SIGKILL, which etude-guest logs as a line that is no frame. */
static const char *const console_keys[] = {"k", "b", "d", "b", "a", "c", "k"};
#define CONSOLE_WORD "kbdback"

/** Boot machine `desktop` into `b`, log in as root and open the guest
 * command channel, as step `boot`. */
static void booted(struct net_case *u, struct net_boot *b)
{
    char password[128];

    net_secret(u, "demo-password", password, sizeof(password));
    if (net_step(u, "boot") == ETUDE_OK)
        net_boot(b, u, "desktop", 1, NULL);
    net_seen(b, "ext2: root: /dev/disk0p1");
    if (net_login(b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b->m) != ETUDE_OK)
        net_fail(u, "the desktop guest command channel did not open");
}

/** Copy artifact `name` to the guest path `guest` with mode `mode`. */
static void put(struct net_case *u, struct net_boot *b, const char *name, const char *guest,
                const char *mode)
{
    if (net_ok(u) && etude_guest_put(b->m, etude_artifact(u->c, name), guest, mode) != ETUDE_OK)
        net_fail(u, "%s did not reach the guest as %s", name, guest);
}

/** Run the guest command `argv` as evidence `output`, and report assertion
 * `id`: its exit 0 with `token` in its output. */
static void check(struct net_case *u, struct net_boot *b, const char *id, const char *const argv[],
                  const char *output, const char *token)
{
    struct net_run r;

    net_guest(u, b->m, argv, NULL, NULL, output, &r);
    net_assert(u, id, net_ok(u) && net_exited0(&r) && strstr(r.out, token) != NULL,
               "%s gave no \"%s\"; see %s and %s.err", argv[1], token, output, output);
    net_run_free(&r);
}

/** Run the display probe's subcommand `command` with `arg`, and report
 * assertion `id`, also the name of its evidence, by its result line
 * `token`. */
static void probe(struct net_case *u, struct net_boot *b, const char *id, const char *command,
                  const char *arg, const char *token)
{
    const char *argv[] = {"/usr/bin/python3", PROBE, command, arg, NULL};

    check(u, b, id, argv, id, token);
}

/** Send QMP's `execute` with `arguments`, failing the case on an error. */
static void qmp(struct net_case *u, struct net_boot *b, const char *execute, const char *arguments)
{
    char *reply = NULL;

    if (net_ok(u) && etude_qmp(b->m, execute, arguments, &reply) != ETUDE_OK)
        net_fail(u, "QMP %s %s: %s", execute, arguments, reply != NULL ? reply : "no reply");
    free(reply);
}

/** Press and release each key of `keys`, `n` of them, as QMP's send-key
 * names them, one command each. */
static void keys(struct net_case *u, struct net_boot *b, const char *const keys[], unsigned n)
{
    char arguments[128];
    unsigned i;

    for (i = 0; i < n; i++) {
        snprintf(arguments, sizeof(arguments),
                 "{\"keys\": [{\"type\": \"qcode\", \"data\": \"%s\"}]}", keys[i]);
        qmp(u, b, "send-key", arguments);
    }
}

/** Send QMP's input-send-event with `events`, a JSON array. */
static void events(struct net_case *u, struct net_boot *b, const char *events)
{
    char arguments[512];

    snprintf(arguments, sizeof(arguments), "{\"events\": %s}", events);
    qmp(u, b, "input-send-event", arguments);
}

/* A guest command that waits for input from the machine. */
struct driven {
    const char *id;          /* its assertion, also the name of its evidence */
    const char *const *argv; /* the command */
    const char *ready;       /* the text it prints when it waits for input */
    const char *token;       /* the text of its success */
    void (*input)(struct net_case *u, struct net_boot *b); /* the input */
};

/** Start the command of `d`, wait for its ready text, run its input on the
 * machine, collect the command, and report its assertion by its token. */
static void driven(struct net_case *u, struct net_boot *b, const struct driven *d)
{
    struct etude_guest_result result;
    struct net_run r;
    const char *path;
    int started = 0;

    memset(&r, 0, sizeof(r));
    if (net_ok(u)) {
        started = etude_guest_start(b->m, d->argv, NULL, NULL) == ETUDE_OK;
        if (!started)
            net_fail(u, "%s did not start", d->argv[1]);
    }
    if (net_ok(u) && etude_guest_output_wait(b->m, d->ready) != ETUDE_OK)
        net_fail(u, "%s never printed \"%s\"", d->argv[1], d->ready);
    if (net_ok(u))
        d->input(u, b);
    if (started && etude_guest_finish(b->m, !net_ok(u), d->id, &result) == ETUDE_OK) {
        path = etude_evidence(u->c, d->id);
        r.end = result.termination;
        r.out = net_file(path != NULL ? path : "");
    }
    net_assert(u, d->id,
               net_ok(u) && r.out != NULL && net_exited0(&r) && strstr(r.out, d->token) != NULL,
               "%s gave no \"%s\"; see %s and %s.err", d->argv[1], d->token, d->id, d->id);
    net_run_free(&r);
}

/** Move the pointer onto the page, which gives Weston the pointer's focus,
 * click the left button, and type the page's keys. Each input waits until
 * the page has reported the one before it: keys sent back to back, with the
 * pointer's move among them, reached the keyboard's event device whole and
 * in order, but the page missed some of them, a click sent just after the
 * move never reached it, and its reports, each a fetch of its own, came in
 * any order. */
static void page_input(struct net_case *u, struct net_boot *b)
{
    char seen[64];
    unsigned i;

    events(u, b,
           "[{\"type\": \"rel\", \"data\": {\"axis\": \"x\", \"value\": 20}}, "
           "{\"type\": \"rel\", \"data\": {\"axis\": \"y\", \"value\": 20}}]");
    if (net_ok(u) && etude_guest_output_wait(b->m, "desktop: pointer: on the page") != ETUDE_OK)
        net_fail(u, "the page reported no pointer");
    events(u, b, "[{\"type\": \"btn\", \"data\": {\"down\": true, \"button\": \"left\"}}]");
    events(u, b, "[{\"type\": \"btn\", \"data\": {\"down\": false, \"button\": \"left\"}}]");
    if (net_ok(u) && etude_guest_output_wait(b->m, "desktop: click: button 0") != ETUDE_OK)
        net_fail(u, "the page reported no click");
    for (i = 0; i < sizeof(page_keys) / sizeof(page_keys[0]); i++) {
        keys(u, b, &page_keys[i], 1);
        snprintf(seen, sizeof(seen), "desktop: key: %s", page_keys[i]);
        if (net_ok(u) && etude_guest_output_wait(b->m, seen) != ETUDE_OK)
            net_fail(u, "the page reported no key %s", page_keys[i]);
    }
}

/** Show the probe's input page in browser `name`, inject the page's input
 * through QMP and report assertion `name` by what the page recorded. */
static void browser(struct net_case *u, struct net_boot *b, const char *name)
{
    const char *argv[] = {"/usr/bin/python3", PROBE, "browser", name, NULL};
    char ready[64], token[96];
    struct driven d;

    snprintf(ready, sizeof(ready), "desktop: ready: %s", name);
    snprintf(token, sizeof(token), "desktop: result: %s: keys rach2, clicks 1", name);
    d.id = name;
    d.argv = argv;
    d.ready = ready;
    d.token = token;
    d.input = page_input;
    driven(u, b, &d);
}

/** Press evprobe's keys, and when it asks, its flood. */
static void ev_keyboard(struct net_case *u, struct net_boot *b)
{
    keys(u, b, ev_keys, sizeof(ev_keys) / sizeof(ev_keys[0]));
    if (net_ok(u) && etude_guest_output_wait(b->m, "EVPROBE: flood") != ETUDE_OK)
        net_fail(u, "evprobe ps2 never printed \"EVPROBE: flood\"");
    keys(u, b, ev_flood, sizeof(ev_flood) / sizeof(ev_flood[0]));
}

/** Move the mouse by 10 and 5, click, and turn the wheel up and down, each a
 * QMP command of its own so that each is a packet, sent once evprobe has
 * printed that it read the packet of the one before, "EVPROBE: mouse: packet
 * N read", and wait for the last's. QEMU's PS/2 mouse keeps a packet that
 * does not fit its queue of 16 bytes, four of these four-byte packets, until
 * the next event: the five sent together left the wheel's turn down unread
 * on i686 under KVM. */
static void ev_mouse(struct net_case *u, struct net_boot *b)
{
    static const char *const moves[] = {
        "[{\"type\": \"rel\", \"data\": {\"axis\": \"x\", \"value\": 10}}, "
        "{\"type\": \"rel\", \"data\": {\"axis\": \"y\", \"value\": 5}}]",
        "[{\"type\": \"btn\", \"data\": {\"down\": true, \"button\": \"left\"}}]",
        "[{\"type\": \"btn\", \"data\": {\"down\": false, \"button\": \"left\"}}]",
        "[{\"type\": \"btn\", \"data\": {\"down\": true, \"button\": \"wheel-up\"}}, "
        "{\"type\": \"btn\", \"data\": {\"down\": false, \"button\": \"wheel-up\"}}]",
        "[{\"type\": \"btn\", \"data\": {\"down\": true, \"button\": \"wheel-down\"}}, "
        "{\"type\": \"btn\", \"data\": {\"down\": false, \"button\": \"wheel-down\"}}]"};
    char seen[64];
    unsigned i;

    for (i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        events(u, b, moves[i]);
        snprintf(seen, sizeof(seen), "EVPROBE: mouse: packet %u read", i + 1);
        if (net_ok(u) && etude_guest_output_wait(b->m, seen) != ETUDE_OK)
            net_fail(u, "evprobe mouse never printed \"%s\"", seen);
    }
}

/** Check Weston on DRM with `renderer` and the device nodes udev gave the
 * desktop's groups, as steps `drm` and `nodes`. */
static void display(struct net_case *u, struct net_boot *b, const char *renderer)
{
    const char *nodes[] = {"/bin/bash", "-c",
                           "shopt -s nullglob; stat -c '%U:%G %a %n' /dev/dri/card* "
                           "/dev/dri/renderD* /dev/input/event*",
                           NULL};
    char token[64];
    struct net_run r;

    put(u, b, "desktop-display", PROBE, "0755");
    snprintf(token, sizeof(token), "desktop: result: drm: %s renderer, output ", renderer);
    if (net_step(u, "drm") == ETUDE_OK)
        probe(u, b, "drm", "drm", NULL, token);
    if (net_step(u, "nodes") == ETUDE_OK) {
        net_guest(u, b->m, nodes, NULL, NULL, "nodes", &r);
        net_assert(u, "nodes",
                   net_ok(u) && net_exited0(&r) &&
                       strstr(r.out, "root:video 660 /dev/dri/card0") != NULL &&
                       strstr(r.out, "root:input 660 /dev/input/event0") != NULL &&
                       (strstr(r.out, "renderD128") == NULL ||
                        strstr(r.out, "root:render 660 /dev/dri/renderD128") != NULL),
                   "udev did not give the nodes their groups; see nodes");
        net_run_free(&r);
    }
}

/** Press Ctrl+J, which ends any partial line, then the word and Ctrl+J
 * again, so that the console's reader reads the word as a line of its
 * own. */
static void console_input(struct net_case *u, struct net_boot *b)
{
    static const char newline[] = "{\"keys\": [{\"type\": \"qcode\", \"data\": \"ctrl\"}, "
                                  "{\"type\": \"qcode\", \"data\": \"j\"}]}";

    qmp(u, b, "send-key", newline);
    keys(u, b, console_keys, sizeof(console_keys) / sizeof(console_keys[0]));
    qmp(u, b, "send-key", newline);
}

/** Kill Weston and compare the console's frames, then type a word and find
 * the console's reader, etude-guest, received it, as step `console`. The
 * word is typed while a guest command waits for it in etude-guest's log, a
 * command that says nothing until then, so that no frame of the channel
 * crosses the console while the keys come and the word's line stays whole;
 * a frame sent among the keys mixes its bytes into the word's. */
static void console(struct net_case *u, struct net_boot *b)
{
    static const char *const seen[] = {"/bin/sh", "-c",
                                       "w='not a frame: " CONSOLE_WORD
                                       "'; l=/tmp/etude-guest/guest.log; "
                                       "echo 'desktop: console: waiting for the word'; "
                                       "until grep -q \"$w\" $l; do sleep 0.1; done; "
                                       "echo \"desktop: result: keyboard: $(grep -c \"$w\" $l)\"",
                                       NULL};
    static const struct driven word = {"keyboard", seen, "desktop: console: waiting for the word",
                                       "desktop: result: keyboard: 1", console_input};

    put(u, b, "drmcapture", DRMCAPTURE, "0755");
    if (net_step(u, "console") != ETUDE_OK)
        return;
    probe(u, b, "console", "console", DRMCAPTURE, "desktop: result: console: ");
    driven(u, b, &word);
}

/** Weston at boot on bochs with Pixman, both browsers taking input from
 * QMP, and the console after Weston's SIGKILL. */
static int bochs(struct net_case *u)
{
    struct net_boot b;

    booted(u, &b);
    display(u, &b, "pixman");
    if (net_step(u, "browsers") == ETUDE_OK) {
        browser(u, &b, "chromium");
        browser(u, &b, "firefox");
    }
    console(u, &b);
    return net_end(u);
}

/** Weston at boot on virtio-gpu with GL, an EGL client, and two modes set
 * by weston.ini and a restart. */
static int virtio(struct net_case *u)
{
    struct net_boot b;

    booted(u, &b);
    display(u, &b, "gl");
    if (net_step(u, "egl") == ETUDE_OK)
        probe(u, &b, "egl", "egl", NULL, "desktop: result: egl: ");
    if (net_step(u, "modes") == ETUDE_OK) {
        probe(u, &b, "mode-1024", "mode", "1024x768",
              "desktop: result: mode: output Virtual-1, mode 1024x768");
        probe(u, &b, "mode-800", "mode", "800x600",
              "desktop: result: mode: output Virtual-1, mode 800x600");
    }
    return net_end(u);
}

/** On i686: Weston on bochs, the keyboard's and mouse's event devices, an
 * llvmpipe render read back pixel by pixel, and modetest's mode set. */
static int i686(struct net_case *u)
{
    static const char *const ps2[] = {EVPROBE, "ps2", NULL};
    static const char *const pointer[] = {EVPROBE, "mouse", NULL};
    static const struct driven keyboard = {"keyboard", ps2, "EVPROBE: press the keys", "EVPROBE OK",
                                           ev_keyboard};
    static const struct driven mouse = {"mouse", pointer, "EVPROBE: move the mouse", "EVPROBE OK",
                                        ev_mouse};
    const char *modetest[] = {"/bin/bash", "-c",
                              "set -e; python3 " PROBE " kill; m=bochs-drm; "
                              "modetest -M $m -c </dev/null; "
                              "c=$(modetest -M $m -c </dev/null | awk '$3 == \"connected\" "
                              "{ print $1; exit }'); modetest -M $m -s \"$c:1024x768\" </dev/null",
                              NULL};
    struct net_boot b;

    booted(u, &b);
    display(u, &b, "pixman");
    put(u, &b, "evprobe", EVPROBE, "0755");
    if (net_step(u, "input") == ETUDE_OK) {
        driven(u, &b, &keyboard);
        driven(u, &b, &mouse);
    }
    if (net_step(u, "gl") == ETUDE_OK)
        probe(u, &b, "gl", "gl", NULL, "desktop: result: gl: ");
    if (net_step(u, "modetest") == ETUDE_OK)
        check(u, &b, "modetest", modetest, "modetest", "setting mode 1024x768");
    return net_end(u);
}

/** Run the headless case on a private GNU root and retain its output. */
static int headless(struct net_case *u)
{
    struct net_boot b;
    struct net_run r;
    const char *diagnose[] = {"/bin/bash", "-c", "cat /var/log/services; ps -eo pid,ppid,user,args",
                              NULL};
    const char *argv[] = {"/usr/bin/python3", "-", NULL};
    const char *clients[] = {"session", "chromium", "firefox", "alsa"};
    char token[96];
    unsigned i;

    memset(&r, 0, sizeof(r));
    booted(u, &b);
    if (net_step(u, "tones") == ETUDE_OK) {
        net_guest(u, b.m, diagnose, NULL, NULL, "services", &r);
        net_run_free(&r);
    }
    if (net_ok(u))
        net_guest(u, b.m, argv, NULL, etude_artifact(u->c, "desktop-probe"), "tones", &r);
    for (i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
        snprintf(token, sizeof(token), "desktop: result: %s", clients[i]);
        net_assert(u, clients[i], net_ok(u) && net_exited0(&r) && strstr(r.out, token) != NULL,
                   "%s did not complete; see tones and tones.err", clients[i]);
    }
    net_run_free(&r);
    return net_end(u);
}

/* The family's cases, by the part of their names after the last dot. */
static const struct {
    const char *name;
    int (*run)(struct net_case *u);
} cases[] = {
    {"headless", headless},
    {"bochs", bochs},
    {"virtio", virtio},
    {"i686", i686},
};

/**
 * Run the case of `c` by its name.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');
    unsigned i;

    net_case(&u, c);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (dot != NULL && strcmp(dot + 1, cases[i].name) == 0)
            return cases[i].run(&u);
    net_fail(&u, "no case %s", id);
    return net_end(&u);
}
