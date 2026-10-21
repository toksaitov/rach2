/*
 * usb_scenario.c: the scenarios of tests/cases/machine.usb.json, the USB
 * stack from a port to a keystroke, each case a boot of its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbcase.h"

/* The family's cases begin with this, and their scenarios are named by the
 * rest. */
#define FAMILY "machine.usb."

/* The kernel's records of a configured keyboard and of a configured mouse. */
#define KEYBOARD "class 3 protocol 1"
#define MOUSE    "class 3 protocol 2"

/* The keys that type `help` at the shell, and the shell's answer. */
#define HELP_KEYS   "h e l p ret"
#define HELP_ANSWER "Available commands"

/* The keys each latency case types, one for each of keylatprobe's reads. */
#define LATENCY_KEYS 8

/* The longest path of an artifact or of evidence a scenario keeps. */
#define PATH_MAX_LEN 4096

/** Give the larger of `a` and `b`. */
static uint64_t later(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

/**
 * Begin step `boot` and boot the machine `usb` into `b`; wait for `record`
 * anywhere on its console when it is not NULL, and then for the debug
 * shell's greeting, and report assertion `id`.
 */
static void booted(struct usb_case *u, struct usb_boot *b, const char *record, const char *id)
{
    usb_step(u, "boot");
    usb_boot(b, u, "usb", 1, NULL);
    if (record != NULL)
        usb_seen(b, record);
    usb_wait(b, USB_SHELL);
    usb_assert(u, id, 1, "");
}

/** Type `help` on the machine's keyboard, wait for the shell's answer, and
 * report assertion `id`. */
static void typed(struct usb_case *u, struct usb_boot *b, const char *id)
{
    usb_keys(b, HELP_KEYS);
    usb_wait(b, HELP_ANSWER);
    usb_assert(u, id, 1, "");
}

/** Run `command` at the debug shell, and report assertion `id` of the probe
 * whose words begin with `token`. */
static void probed(struct usb_case *u, struct usb_boot *b, const char *command, const char *token,
                   const char *id)
{
    struct usb_run r;

    usb_exec(b, command, &r);
    usb_probe(u, id, &r, token);
    free(r.text);
}

/** Report assertion `id`: held when the console of `b` up to `to` has no
 * line with `text`, else failed with the first such line. */
static void never(struct usb_case *u, const struct usb_boot *b, uint64_t to, const char *text,
                  const char *id)
{
    char *console = usb_text(b, 0, to);
    char line[160];
    const char *found = usb_find(console, text);

    usb_assert(u, id, found == NULL, "%s", usb_copy_line(found, line, sizeof(line)));
    free(console);
}

/** Count the lines of `text` that record a configured device, or one
 * refused for want of a driver: `usb: device: port P: ... class`, P a root
 * port or a path behind hubs. */
static unsigned configured(const char *text)
{
    return usb_count(text, "usb: device: port #: *class") +
           usb_count(text, "usb: device: port #.#: *class") +
           usb_count(text, "usb: device: port #.#.#: *class") +
           usb_count(text, "usb: device: port #.#.#.#: *class");
}

/** Give the number after the first `prefix` of the first line of `text`
 * that has `pattern` in it, or -1. */
static int number_in(const char *text, const char *pattern, const char *prefix)
{
    char line[200];
    const char *p;

    usb_copy_line(usb_find(text, pattern), line, sizeof(line));
    p = strstr(line, prefix);
    if (p == NULL)
        return -1;
    p += strlen(prefix);
    return *p >= '0' && *p <= '9' ? atoi(p) : -1;
}

/** Copy `path` into `buf` of PATH_MAX_LEN bytes, since the library's paths
 * are overwritten by its next call; an empty text for NULL. */
static char *keep_path(char *buf, const char *path)
{
    snprintf(buf, PATH_MAX_LEN, "%s", path != NULL ? path : "");
    return buf;
}

/** Plug in the stick `st1` on `xhci.0`, at `port` when it is not NULL, its
 * medium a new node `again` of the copy of artifact `stick`. */
static void replug_stick(struct usb_case *u, struct usb_boot *b, const char *port)
{
    char more[100];

    usb_medium(b, "again", etude_private_copy(u->c, "stick"), 0);
    if (port != NULL)
        snprintf(more, sizeof(more), "\"port\": \"%s\", \"drive\": \"again\"", port);
    else
        snprintf(more, sizeof(more), "\"drive\": \"again\"");
    usb_plug(b, "usb-storage", "xhci.0", "st1", more);
}

/** Run `command` at the debug shell, pull device `device` once the probe
 * prints `USBDISKPROBE HOLDING`, and report assertion `id` of its run. */
static void pulled_under(struct usb_case *u, struct usb_boot *b, const char *command,
                         const char *device, const char *id)
{
    struct usb_run r;
    uint64_t from = usb_start(b, command);

    usb_wait(b, "USBDISKPROBE HOLDING");
    usb_pull(b, device);
    usb_finish(b, from, &r);
    usb_probe(u, id, &r, "USBDISKPROBE");
    free(r.text);
}

/** Boot to the shell and run usbprobe as `command`, as assertion `id`; with
 * `count`, also require `USBPROBE settled` and that many configured
 * records. */
static int listing(struct usb_case *u, const char *command, const char *id, unsigned count)
{
    struct usb_boot b;
    struct usb_run r;
    char *text;
    unsigned n;

    booted(u, &b, NULL, "shell");
    usb_step(u, "probe");
    usb_exec(&b, command, &r);
    if (count > 0 && r.text != NULL && usb_find(r.text, "USBPROBE FAIL") == NULL &&
        usb_find(r.text, "USBPROBE settled") == NULL)
        usb_fail(u, "usbprobe did not say USBPROBE settled");
    usb_probe(u, id, &r, "USBPROBE");
    free(r.text);
    if (count > 0) {
        text = usb_text(&b, 0, b.at);
        n = configured(text);
        usb_assert(u, "configured", n == count, "%u devices configured, not %u", n, count);
        free(text);
    }
    return usb_end(u);
}

/** The keyboard and the mouse listed by usbprobe. */
static int listed(struct usb_case *u)
{
    return listing(u, "usbprobe", "listed", 0);
}

/** With `rach2.nousb` /proc/usb is there and empty. */
static int empty(struct usb_case *u)
{
    return listing(u, "usbprobe", "empty", 0);
}

/** Sixteen devices behind two hubs, every one configured. */
static int sixteen(struct usb_case *u)
{
    return listing(u, "usbprobe 16", "settled", 16);
}

/** A key typed on the USB keyboard reaches the shell. */
static int key_shell(struct usb_case *u)
{
    struct usb_boot b;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "type");
    typed(u, &b, "typed");
    return usb_end(u);
}

/** A program reads the keys through the line discipline. */
static int key_read(struct usb_case *u)
{
    struct usb_boot b;
    struct usb_run r;
    uint64_t from;
    int lines, passed;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "read");
    from = usb_start(&b, "/stdintest");
    usb_wait(&b, "Test 1: read from stdin");
    usb_keys(&b, "a b ret c d ret e f ret");
    usb_finish(&b, from, &r);
    lines = r.text != NULL && usb_find(r.text, "Read 2 bytes: \"ab\"") != NULL &&
            usb_find(r.text, "Read 2 bytes: \"cd\"") != NULL;
    passed = r.text != NULL && usb_find(r.text, "All stdin syscalls passed") != NULL;
    usb_assert(u, "read", lines && passed && r.status == 0,
               "stdintest exited with status %d, %s ab and cd, %s", r.status,
               lines ? "read" : "did not read", passed ? "passed" : "did not pass");
    free(r.text);
    return usb_end(u);
}

/** A movement arrives at the mouse device. */
static int mouse_move(struct usb_case *u)
{
    struct usb_boot b;
    struct usb_run r;
    uint64_t from;

    booted(u, &b, MOUSE, "mouse");
    usb_step(u, "move");
    from = usb_start(&b, "/mouseprobe");
    usb_wait(&b, "MOUSEPROBE: move the mouse");
    usb_mouse(&b, 20, -8, 0);
    usb_finish(&b, from, &r);
    usb_probe(u, "moved", &r, "MOUSEPROBE");
    free(r.text);
    return usb_end(u);
}

/** A keyboard plugged in after the boot, and pulled out. */
static int hotplug(struct usb_case *u)
{
    struct usb_boot b;

    booted(u, &b, NULL, "shell");
    usb_step(u, "plug");
    usb_plug(&b, "usb-kbd", "xhci.0", "later", NULL);
    usb_wait(&b, "usb: device: port 5:");
    usb_assert(u, "added", 1, "");
    usb_step(u, "pull");
    usb_pull(&b, "later");
    usb_wait(&b, "port 5: gone");
    usb_assert(u, "removed", 1, "");
    return usb_end(u);
}

/** The mouse replugged beside a device that cannot be driven. */
static int replug(struct usb_case *u)
{
    struct usb_boot b;

    usb_step(u, "boot");
    usb_boot(&b, u, "usb", 1, NULL);
    usb_wait(&b, "usb: devices: 2 configured");
    usb_assert(u, "configured", 1, "");
    usb_step(u, "pull");
    usb_pull(&b, "themouse");
    usb_wait(&b, ": gone");
    usb_assert(u, "gone", 1, "");
    usb_step(u, "plug");
    usb_plug(&b, "usb-mouse", "xhci.0", "themouse", NULL);
    usb_wait(&b, "class 3 protocol 2, endpoint 1");
    usb_assert(u, "again", 1, "");
    return usb_end(u);
}

/** Run intrprobe, as assertions `counted` and `no-failure`. */
static void interrupts(struct usb_case *u, struct usb_boot *b)
{
    usb_step(u, "probe");
    probed(u, b, "/intrprobe", "INTRPROBE", "counted");
    never(u, b, b->at, "INTRPROBE FAIL", "no-failure");
}

/** Keys typed on the USB keyboard, then the interrupt counts read. */
static int typed_counts(struct usb_case *u)
{
    struct usb_boot b;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "type");
    typed(u, &b, "typed");
    interrupts(u, &b);
    return usb_end(u);
}

/** The mouse moved with three button states, then the counts read. */
static int mouse_counts(struct usb_case *u)
{
    struct usb_boot b;

    booted(u, &b, MOUSE, "mouse");
    usb_step(u, "move");
    usb_mouse(&b, 20, -8, 0);
    usb_mouse(&b, -12, 6, 1);
    usb_mouse(&b, 8, 8, 2);
    interrupts(u, &b);
    return usb_end(u);
}

/** The PS/2 keyboard's line and the timer, with messages on. */
static int line_counts(struct usb_case *u)
{
    struct usb_boot b;

    booted(u, &b, NULL, "shell");
    usb_step(u, "type");
    typed(u, &b, "typed");
    interrupts(u, &b);
    return usb_end(u);
}

/**
 * Run usblatency.awk on the console of `b` for the mode `random`, and
 * report assertion `records`: held when it exits 0. Print the records for
 * a person, as plain lines.
 */
static void judge_latency(struct usb_case *u, struct usb_boot *b, int random)
{
    struct etude_termination end;
    char awk[PATH_MAX_LEN], line[200];
    char *argv[] = {"awk", "-v", "random=0", "-v", "keys=8", "-f", awk, b->console, NULL};
    enum etude_status st;
    char *text;
    const char *p;

    if (random)
        argv[2] = "random=1";
    usb_step(u, "judge");
    if (!usb_ok(u)) {
        usb_assert(u, "records", 0, "");
        return;
    }
    keep_path(awk, etude_artifact(u->c, "usblatency.awk"));
    st = etude_host(u->c, argv, "usblatency", &end);
    if (st != ETUDE_OK) {
        usb_fail(u, "awk: %s", st == ETUDE_BOUND ? "the step's bound expired" : "did not run");
        usb_assert(u, "records", 0, "");
        return;
    }
    text = usb_text(b, 0, b->at);
    for (p = usb_find(text, "usb: latency: msi"); p != NULL;
         p = usb_find(p + strcspn(p, "\r\n"), "usb: latency: msi"))
        printf("usb: latency: %s: %s\n", random ? "random" : "spin",
               usb_copy_line(strstr(p, "msi"), line, sizeof(line)));
    free(text);
    usb_assert(u, "records", end.end == ETUDE_END_EXITED && end.value == 0, "usblatency.awk %s %d",
               end.end == ETUDE_END_EXITED ? "exited with status" : "ended by signal", end.value);
}

/**
 * Eight keys typed to keylatprobe under `rach2.usblatency`, each once
 * the kernel says the reader waits, beside a spinning child or, with
 * `random`, a child inside getrandom.
 */
static int latency(struct usb_case *u, int random)
{
    struct usb_boot b;
    struct usb_run r;
    uint64_t from, ready, waiting, last;
    int i;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "read");
    from = usb_start(&b, random ? "/keylatprobe random 8" : "/keylatprobe spin 8");
    usb_after(&b, random ? "KEYLAT HOG" : "KEYLAT READING", from, &ready);
    usb_after(&b, "usb: latency: reader waiting", from, &waiting);
    b.at = later(ready, waiting);
    usb_keys(&b, "ret");
    for (i = 1; i < LATENCY_KEYS; i++) {
        usb_wait(&b, "usb: latency: msi");
        usb_wait(&b, "usb: latency: reader waiting");
        usb_keys(&b, "ret");
    }
    usb_wait(&b, "usb: latency: msi");
    usb_assert(u, "keys", 1, "");
    last = b.at;
    usb_finish(&b, from, &r);
    usb_probe(u, "read", &r, "KEYLAT");
    free(r.text);
    never(u, &b, later(last, b.at), "KEYLAT FAIL", "no-failure");
    never(u, &b, later(last, b.at), "usb: pump:", "no-pump");
    judge_latency(u, &b, random);
    return usb_end(u);
}

/** The keystroke's path beside a spinning child. */
static int latency_spin(struct usb_case *u)
{
    return latency(u, 0);
}

/** The keystroke's path beside a child inside getrandom. */
static int latency_random(struct usb_case *u)
{
    return latency(u, 1);
}

/** Without `rach2.usblatency` the key is read and no record printed. */
static int latency_off(struct usb_case *u)
{
    struct usb_boot b;
    struct usb_run r;
    uint64_t from;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "read");
    from = usb_start(&b, "/keylatprobe");
    usb_wait(&b, "KEYLAT READING");
    usb_keys(&b, "ret");
    usb_finish(&b, from, &r);
    usb_probe(u, "read", &r, "KEYLAT");
    free(r.text);
    never(u, &b, b.at, "usb: latency:", "unrecorded");
    return usb_end(u);
}

/** A keyboard and a mouse behind a hub. */
static int hub_tree(struct usb_case *u)
{
    struct usb_boot b;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "type");
    typed(u, &b, "typed");
    usb_step(u, "probe");
    probed(u, &b, "/usbprobe", "USBPROBE", "listed");
    never(u, &b, b.at, "USBPROBE FAIL", "no-failure");
    return usb_end(u);
}

/** A stick behind the hub written, pulled and plugged back in. */
static int hub_stick(struct usb_case *u)
{
    struct usb_boot b;

    booted(u, &b, NULL, "shell");
    usb_step(u, "write");
    probed(u, &b, "/usbdiskprobe write", "USBDISKPROBE", "written");
    usb_step(u, "pull");
    pulled_under(u, &b, "/usbdiskprobe hold", "st0", "pulled");
    usb_step(u, "replug");
    replug_stick(u, &b, "1.4");
    usb_wait(&b, "usbdisk: disk0: mbr");
    usb_assert(u, "enumerated", 1, "");
    probed(u, &b, "/usbdiskprobe replug", "USBDISKPROBE", "replugged");
    never(u, &b, b.at, "USBDISKPROBE FAIL", "no-failure");
    return usb_end(u);
}

/**
 * A read in flight when the stick is pulled. The medium is the
 * stick's copy behind a throttle of a 32 KiB budget and a byte a second
 * after it, which a profile's disk cannot give, so the stick is plugged in
 * after the boot.
 */
static int pull_read(struct usb_case *u)
{
    struct usb_boot b;
    struct usb_run r;
    char path[PATH_MAX_LEN], line[160];
    uint64_t from, kept;
    char *text;
    const char *found;

    booted(u, &b, NULL, "shell");
    usb_step(u, "plug");
    keep_path(path, etude_private_copy(u->c, "stick"));
    usb_qmp(&b, "object-add",
            "{\"qom-type\": \"throttle-group\", \"id\": \"tgs\", \"x-bps-total\": 1, "
            "\"x-bps-total-max\": 32768}");
    usb_qmp(
        &b, "blockdev-add",
        "{\"driver\": \"throttle\", \"node-name\": \"stick\", \"throttle-group\": \"tgs\", "
        "\"file\": {\"driver\": \"raw\", \"node-name\": \"stickraw\", "
        "\"file\": {\"driver\": \"file\", \"node-name\": \"stickfile\", \"filename\": \"%s\"}}}",
        path);
    usb_plug(&b, "usb-storage", "xhci.0", "st0", "\"drive\": \"stick\"");
    usb_wait(&b, "usbdisk: disk0:");
    usb_assert(u, "plugged", 1, "");
    usb_step(u, "read");
    from = usb_start(&b, "/usbdiskprobe pullread");
    usb_wait(&b, "USBDISKPROBE READING");
    kept = b.at;
    usb_pull(&b, "st0");
    usb_wait(&b, "its request is kept, its ");
    usb_wait(&b, " in flight");
    text = usb_text(&b, kept, b.at);
    found = usb_last(text, "its request is kept, its * in flight");
    usb_assert(u, "kept", found != NULL, "the line is %s",
               usb_copy_line(usb_last(text, "its request is kept"), line, sizeof(line)));
    free(text);
    usb_wait(&b, ": given back");
    usb_assert(u, "given-back", 1, "");
    usb_wait(&b, "its request is failed with the slot given back");
    usb_assert(u, "failed-after", 1, "");
    usb_wait(&b, "USBDISKPROBE READ FAILED");
    usb_finish(&b, from, &r);
    usb_probe(u, "read-failed", &r, "USBDISKPROBE");
    free(r.text);
    usb_step(u, "replug");
    replug_stick(u, &b, NULL);
    usb_wait(&b, ": mbr");
    usb_assert(u, "enumerated", 1, "");
    never(u, &b, b.at, "USBDISKPROBE FAIL", "no-failure");
    never(u, &b, b.at, "a transfer never answered", "answered");
    return usb_end(u);
}

/** A stick given the slot of one pulled while its file is open. */
static int reslot(struct usb_case *u)
{
    struct usb_boot b;
    struct usb_run r;
    uint64_t from;

    booted(u, &b, NULL, "shell");
    usb_step(u, "keep");
    from = usb_start(&b, "/usbdiskprobe keep");
    usb_wait(&b, "USBDISKPROBE KEEPING");
    usb_pull(&b, "st0");
    usb_wait(&b, ": given back");
    usb_assert(u, "given-back", 1, "");
    replug_stick(u, &b, NULL);
    usb_wait(&b, ": mbr");
    usb_assert(u, "enumerated", 1, "");
    usb_finish(&b, from, &r);
    usb_probe(u, "kept", &r, "USBDISKPROBE");
    free(r.text);
    never(u, &b, b.at, "USBDISKPROBE FAIL", "no-failure");
    return usb_end(u);
}

/** The hub pulled with a keyboard, a mouse and a stick behind it. */
static int hub_pull(struct usb_case *u)
{
    struct usb_boot b;
    uint64_t slots, hub, to;
    char *text;
    const char *last;
    unsigned below, back;
    char line[160];

    booted(u, &b, NULL, "shell");
    usb_step(u, "pull");
    pulled_under(u, &b, "/usbdiskprobe hold", "hub", "unmounted");
    usb_times(&b, "usb: slot: ", 4, &slots);
    usb_times(&b, ": gone", 4, &hub);
    to = later(b.at, later(slots, hub));
    text = usb_text(&b, 0, to);
    below = usb_count(text, "usb: device: port #.#: gone");
    last = usb_last(text, "gone");
    usb_assert(u, "below-first",
               below == 3 && last != NULL && usb_match(last, strcspn(last, "\r\n"), "port #: gone"),
               "%u gone below the hub, the last: %s", below,
               usb_copy_line(last, line, sizeof(line)));
    back = usb_count(text, "usb: slot: #: given back");
    usb_assert(u, "slots", back == 4 && usb_find(text, "the entry is lost") == NULL,
               "%u slots given back%s", back,
               usb_find(text, "the entry is lost") != NULL ? ", and an entry lost" : "");
    free(text);
    never(u, &b, to, "USBDISKPROBE FAIL", "no-failure");
    return usb_end(u);
}

/** A keyboard behind a hub behind a hub. */
static int hub_hub(struct usb_case *u)
{
    struct usb_boot b;
    char *text;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "type");
    typed(u, &b, "typed");
    text = usb_text(&b, 0, b.at);
    usb_assert(u, "path",
               usb_find(text, "usb: device: port #.1.2: 627:1, class 3 protocol 1") != NULL,
               "no keyboard at port R.1.2");
    free(text);
    return usb_end(u);
}

/** Sixteen devices fill the table, and the seventeenth is refused. */
static int table_full(struct usb_case *u)
{
    struct usb_boot b;
    uint64_t at;
    char *text;
    unsigned n;

    booted(u, &b, "the device table is full", "refused");
    usb_step(u, "help");
    usb_line(&b, "help");
    usb_wait(&b, HELP_ANSWER);
    usb_assert(u, "answered", 1, "");
    usb_times(&b, ", class ", 16, &at);
    text = usb_text(&b, 0, later(at, b.at));
    n = configured(text);
    usb_assert(u, "configured", n == 16, "%u devices configured", n);
    free(text);
    return usb_end(u);
}

/** Two controllers, the only keyboard behind a hub on the second. */
static int two_hc(struct usb_case *u)
{
    struct usb_boot b;
    uint64_t at;
    char *text;
    unsigned ready, n;
    int v0, v1, pumped;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "type");
    typed(u, &b, "typed");
    usb_times(&b, ", class ", 4, &at);
    text = usb_text(&b, 0, later(at, b.at));
    ready = usb_count(text, "usb: ready: xhci0:") + usb_count(text, "usb: ready: xhci1:");
    usb_assert(u, "controllers", ready == 2, "%u controllers ready", ready);
    n = usb_count(text, "usb: device: port *class");
    usb_assert(u, "devices", n == 4, "%u devices configured", n);
    v0 = number_in(text, "interrupts: xhci0: *vector #", "vector ");
    v1 = number_in(text, "interrupts: xhci1: *vector #", "vector ");
    pumped = usb_find(text, "the timer pumps this controller") != NULL;
    usb_assert(u, "vectors", v0 >= 0 && v1 >= 0 && v0 != v1 && !pumped, "vectors %d and %d%s", v0,
               v1, pumped ? ", and a controller pumped" : "");
    free(text);
    return usb_end(u);
}

/** Three sticks on two controllers, and the pull of st1 under `command`. */
static int two_sticks(struct usb_case *u, const char *command)
{
    struct usb_boot b;

    booted(u, &b, NULL, "shell");
    usb_step(u, "three");
    probed(u, &b, "/usbdiskprobe three", "USBDISKPROBE", "mounted");
    usb_step(u, "pull");
    pulled_under(u, &b, command, "st1", "pulled");
    never(u, &b, b.at, "USBDISKPROBE FAIL", "no-failure");
    return usb_end(u);
}

/** One stick on the first controller and two on the second: `/dev/disk1`
 * is the second controller's first. */
static int two_sticks_second(struct usb_case *u)
{
    return two_sticks(u, "/usbdiskprobe pullone");
}

/** Two sticks on the first controller and one on the second: `/dev/disk2`
 * is the first controller's second. */
static int two_sticks_first(struct usb_case *u)
{
    return two_sticks(u, "/usbdiskprobe pullone 2");
}

/** One controller refused at its bring-up, the other serving. */
static int hc_refused(struct usb_case *u)
{
    struct usb_boot b;
    char *text;
    unsigned ready, n;
    int refused;

    booted(u, &b, KEYBOARD, "keyboard");
    usb_step(u, "type");
    typed(u, &b, "typed");
    text = usb_text(&b, 0, b.at);
    ready = usb_count(text, "usb: ready: xhci");
    refused = usb_find(text, "usb: refused: xhci0: rach2.hcfail=0") != NULL;
    usb_assert(u, "refused", refused && ready == 1, "xhci0 %s, %u controllers ready",
               refused ? "refused" : "not refused", ready);
    n = usb_count(text, "usb: device: port *class");
    usb_assert(u, "devices", n == 1, "%u devices configured", n);
    free(text);
    return usb_end(u);
}

/* Each case's scenario, by the last part of its identity. */
static const struct {
    const char *name;
    int (*run)(struct usb_case *u);
} scenarios[] = {
    {"enum-high", listed},
    {"enum-full", listed},
    {"key-shell", key_shell},
    {"key-read", key_read},
    {"mouse-move", mouse_move},
    {"hotplug", hotplug},
    {"off", empty},
    {"sixteen", sixteen},
    {"slow-scan", sixteen},
    {"undrivable", listed},
    {"replug", replug},
    {"msi", typed_counts},
    {"nomsi", typed_counts},
    {"mouse-msi", mouse_counts},
    {"msi-drop", typed_counts},
    {"msi-lines", line_counts},
    {"latency-spin", latency_spin},
    {"latency-random", latency_random},
    {"latency-off", latency_off},
    {"hub-tree", hub_tree},
    {"hub-stick", hub_stick},
    {"pull-read", pull_read},
    {"reslot", reslot},
    {"hub-pull", hub_pull},
    {"hub-hub", hub_hub},
    {"table-full", table_full},
    {"two-hc", two_hc},
    {"two-hc-sticks", two_sticks_second},
    {"two-hc-sticks-first", two_sticks_first},
    {"hc-refused", hc_refused},
};

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);
    struct usb_case u;
    size_t i;

    usb_case(&u, c);
    if (strncmp(id, FAMILY, strlen(FAMILY)) != 0)
        return 1;
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(id + strlen(FAMILY), scenarios[i].name) == 0)
            return scenarios[i].run(&u);
    }
    return 1;
}
