/*
 * keyboard_scenario.c: the scenario of tests/cases/machine.keyboard.json:
 * keys pressed on QEMU's PS/2 or USB keyboard, through the console's keymap,
 * with kbd's tools at os-real's root shell.
 *
 * Each case is one boot. Root loads the Kyrgyz layout with its Alt+Shift
 * switch, `loadkeys -u kg alt-shift`, and runs `showkey -a`, which reads the
 * console with no echo processing of its own, ISIG and ICRNL off, and prints
 * every byte it gets in decimal, octal and hex; the scenario presses keys
 * through QMP's send-key and reads the bytes back off the console. A key held
 * down by input-send-event is released only once the console has shown six
 * of its repeats, so the repeats are the kernel's own, counted without a
 * clock. Caps Lock is then pressed with an A after it, and once showkey has
 * read the A, its LED is read with `setleds`, which asks KDGETLED, and in
 * QEMU: the PS/2 case enables QEMU's trace event ps2_set_ledstate into
 * a log file of the attempt's evidence, and the USB case plugs its keyboard
 * in after the login with QEMU's capture of its packets, in which the LEDs
 * are SET_REPORT requests of one byte. Last, `showkey` in its key code mode,
 * which takes the keyboard in K_MEDIUMRAW with VMIN and VTIME set, and
 * `kbdrate`, which sets the repeat with KDKBDREP and reports the times it
 * replaced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The machine of both cases, and root's prompt. */
#define MACHINE "guest"
#define PROMPT  "# "

/* The kernel's record of a configured USB keyboard. */
#define USB_KEYBOARD "class 3 protocol 1"

/* What showkey -a prints first, and the line of each byte it reads. */
#define SHOWKEY_READY "Ctrl-D will terminate this program"
#define BYTE_A        " 97 0141 0x61"
#define BYTE_CAPS_A   " 65 0101 0x41"
#define BYTE_CR       " 13 0015 0x0d"
#define BYTE_EOT      "  4 0004 0x04"

/* The repeats of the held key the console must show before its release. */
#define REPEATS 6

/*
 * The keys pressed under showkey -a, as send-key's words, and the bytes the
 * Kyrgyz layout gives for them: A and Shift+A on the US layout, the switch
 * to Kyrgyz, its ef, U+0444, in lower and upper case in UTF-8, the capital
 * again under Caps Lock, Ctrl+C on the Cyrillic layer, the switch back to A,
 * and Enter.
 */
#define LAYOUT_KEYS "a shift-a alt-shift_r a shift-a caps_lock a caps_lock ctrl-c alt-shift_r a ret"
static const unsigned char layout_bytes[] = {0x61, 0x41, 0xD1, 0x84, 0xD0, 0xA4,
                                             0xD0, 0xA4, 0x03, 0x61, 0x0D};

/* What one attempt shares among its steps. */
struct kb_case {
    struct net_case u;
    struct net_boot b;
    int usb;            /* the USB case, else the PS/2 one */
    char password[128]; /* root's, the demo password */
    char seen[4096];    /* QEMU's view of the LEDs: the trace or the capture */
};

/** Say whether `text` has `what` in it; NULL has nothing. */
static int has(const char *text, const char *what)
{
    return text != NULL && strstr(text, what) != NULL;
}

/**
 * Write `in` into `buf` of `size` bytes as a JSON string without its quotes,
 * a path that may hold Windows' backslashes; fail the attempt when it does
 * not fit.
 */
static const char *json_text(struct kb_case *s, const char *in, char *buf, size_t size)
{
    size_t n = 0;

    for (; *in != '\0'; in++) {
        if (n + 3 > size) {
            net_fail(&s->u, "the path is too long for QMP: %.60s", in);
            return NULL;
        }
        if (*in == '\\' || *in == '"')
            buf[n++] = '\\';
        buf[n++] = *in;
    }
    buf[n] = '\0';
    return buf;
}

/**
 * Send QMP command `execute` with `arguments`, and require `return`; when
 * `quiet` is set, require it to be empty, as the answer of a human monitor
 * command that succeeded is.
 */
static void qmp(struct kb_case *s, const char *execute, const char *arguments, int quiet)
{
    enum etude_status st;
    char *reply = NULL;

    if (!net_ok(&s->u))
        return;
    st = etude_qmp(s->b.m, execute, arguments, &reply);
    if (st != ETUDE_OK ||
        (quiet && !has(reply, "\"return\": \"\"") && !has(reply, "\"return\":\"\"")))
        net_fail(&s->u, "QMP %s %s: %.120s", execute, arguments, reply != NULL ? reply : "");
    free(reply);
}

/** Run the human monitor's `command`. */
static void hmp(struct kb_case *s, const char *command)
{
    char args[4200], text[4150];

    if (json_text(s, command, text, sizeof(text)) == NULL)
        return;
    snprintf(args, sizeof(args), "{\"command-line\":\"%s\"}", text);
    qmp(s, "human-monitor-command", args, 1);
}

/**
 * Press each key combination of `keys`, words separated by spaces whose
 * keys are joined by `-`, as QEMU's qcodes: send-key presses a word's keys
 * in order and releases them, and QEMU queues the words in turn.
 */
static void press(struct kb_case *s, const char *keys)
{
    char args[1024], word[64];
    const char *p = keys, *end, *dash;
    size_t n, w, i, len;

    while (*p != '\0' && net_ok(&s->u)) {
        end = strchr(p, ' ');
        w = end != NULL ? (size_t)(end - p) : strlen(p);
        if (w >= sizeof(word)) {
            net_fail(&s->u, "the key %.20s is too long", p);
            return;
        }
        memcpy(word, p, w);
        word[w] = '\0';
        n = (size_t)snprintf(args, sizeof(args), "{\"keys\":[");
        for (i = 0; i < w; i += len + 1) {
            dash = strchr(word + i, '-');
            len = dash != NULL ? (size_t)(dash - (word + i)) : w - i;
            n += (size_t)snprintf(args + n, sizeof(args) - n,
                                  "%s{\"type\":\"qcode\",\"data\":\"%.*s\"}", i == 0 ? "" : ",",
                                  (int)len, word + i);
        }
        snprintf(args + n, sizeof(args) - n, "]}");
        qmp(s, "send-key", args, 0);
        p += w;
        while (*p == ' ')
            p++;
    }
}

/** Hold the key of qcode `key` down, or let it up, as one event that
 * bypasses send-key's queue. */
static void hold(struct kb_case *s, const char *key, int down)
{
    char args[256];

    snprintf(args, sizeof(args),
             "{\"events\":[{\"type\":\"key\",\"data\":{\"down\":%s,\"key\":{\"type\":\"qcode\","
             "\"data\":\"%s\"}}}]}",
             down ? "true" : "false", key);
    qmp(s, "input-send-event", args, 0);
}

/**
 * Store in `out`, of `size` bytes, the bytes showkey -a reported in `text`,
 * each from a line ending in its octal and hex values, `0ooo 0xhh`, and give
 * their count.
 */
static size_t bytes_of(const char *text, unsigned char *out, size_t size)
{
    const char *p = text;
    size_t n = 0;
    unsigned v;

    while (text != NULL && (p = strstr(p, " 0x")) != NULL && n < size) {
        if (p - text >= 5 && p[-4] == '0' && sscanf(p + 3, "%2x", &v) == 1)
            out[n++] = (unsigned char)v;
        p += 3;
    }
    return n;
}

/** Log in, after plugging in the USB case's keyboard with its capture, and
 * give PS/2's LEDs to the trace in the PS/2 case. */
static void boot(struct kb_case *s)
{
    char args[4400], path[4150];

    net_step(&s->u, "boot");
    net_boot(&s->b, &s->u, MACHINE, 1, NULL);
    if (net_path(&s->u, s->usb ? "usb-keyboard.pcap" : "ps2-leds.trace", s->seen,
                 sizeof(s->seen)) == NULL)
        return;
    if (!s->usb) {
        snprintf(args, sizeof(args), "logfile %s", s->seen);
        hmp(s, args);
        hmp(s, "trace-event ps2_set_ledstate on");
    }
    net_login(&s->b, "root", s->password, PROMPT);
    if (s->usb && json_text(s, s->seen, path, sizeof(path)) != NULL) {
        snprintf(args, sizeof(args),
                 "{\"driver\":\"usb-kbd\",\"bus\":\"xhci.0\",\"id\":\"keyboard\",\"pcap\":\"%s\"}",
                 path);
        qmp(s, "device_add", args, 0);
        net_seen(&s->b, USB_KEYBOARD);
    }
    net_assert(&s->u, "shell", net_ok(&s->u), "");
}

/** Load the Kyrgyz layout with its Alt+Shift switch. */
static void load(struct kb_case *s)
{
    char *text = NULL;

    net_step(&s->u, "load");
    net_command(&s->b, "loadkeys -u kg alt-shift; echo LOAD=$? LOAD\"\"DONE", "LOADDONE", &text);
    net_assert(&s->u, "loaded", has(text, "LOAD=0"), "loadkeys -u kg alt-shift failed: %.200s",
               text != NULL ? text : "");
    free(text);
}

/** Press the layout's keys and hold A under showkey -a, and judge the bytes
 * and the repeats. */
static void keys(struct kb_case *s)
{
    unsigned char got[256];
    char *text = NULL;
    uint64_t from, at = 0;
    size_t n, i, repeats = 0;
    unsigned before;

    net_step(&s->u, "keys");
    net_line(&s->b, "showkey -a");
    net_wait(&s->b, SHOWKEY_READY);
    from = s->b.at;
    press(s, LAYOUT_KEYS);
    net_wait(&s->b, BYTE_CR);
    text = net_text(&s->b, 0, s->b.at);
    before = net_count(text, BYTE_A);
    free(text);
    hold(s, "a", 1);
    net_times(&s->b, BYTE_A, before + 1 + REPEATS, &at);
    hold(s, "a", 0);
    press(s, "ctrl-d");
    net_wait(&s->b, BYTE_EOT);
    net_wait(&s->b, PROMPT);
    text = net_text(&s->b, from, s->b.at);
    n = bytes_of(text, got, sizeof(got));
    free(text);
    net_assert(&s->u, "layout",
               n >= sizeof(layout_bytes) && memcmp(got, layout_bytes, sizeof(layout_bytes)) == 0,
               "showkey -a read %u bytes, not a, A, the Cyrillic ef, its capital twice, ETX, a "
               "and CR from the keys",
               (unsigned)n);
    for (i = sizeof(layout_bytes); i < n && got[i] == 0x61; i++)
        repeats++;
    net_assert(&s->u, "repeat", repeats >= 1 + REPEATS && i + 1 == n && got[i] == 0x04,
               "the held A gave %u bytes before Ctrl+D, not its press and at least %d repeats",
               (unsigned)repeats, REPEATS);
}

/**
 * Whether the USB keyboard's capture at `path` has the LED report `first`
 * and after it `then`: SET_REPORT, an output report of one byte, in the
 * setup and data of a control transfer QEMU recorded in Linux's usbmon
 * format, link type 220, whose packets each begin with a 64-byte header.
 */
static int reported(const char *path, unsigned first, unsigned then)
{
    static const unsigned char set_report[8] = {0x21, 0x09, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00};
    unsigned char head[24], *pkt = NULL;
    unsigned long len;
    int found = 0;
    FILE *f = fopen(path, "rb");

    if (f == NULL || fread(head, 1, 24, f) != 24 || head[20] != 220) {
        if (f != NULL)
            fclose(f);
        return 0;
    }
    while (found < 2 && fread(head, 1, 16, f) == 16) {
        len = (unsigned long)head[8] | (unsigned long)head[9] << 8 | (unsigned long)head[10] << 16 |
              (unsigned long)head[11] << 24;
        if (len > 1u << 20 || (pkt = realloc(pkt, len + 1)) == NULL || fread(pkt, 1, len, f) != len)
            break;
        /* The usbmon header: transfer type 2 is control, a setup flag of 0
         * says the setup bytes at 40 are valid, and the data follows at 64. */
        if (len >= 65 && pkt[9] == 2 && pkt[14] == 0 && memcmp(pkt + 40, set_report, 8) == 0 &&
            pkt[64] == (found == 0 ? first : then))
            found++;
    }
    free(pkt);
    fclose(f);
    return found == 2;
}

/**
 * Press `keys` and Ctrl+D under showkey -a, and wait until it has read the
 * byte of the line `last` and ended: the console takes the keys in order, so
 * every key before it has then been taken.
 */
static void settle(struct kb_case *s, const char *keys, const char *last)
{
    net_line(&s->b, "showkey -a");
    net_wait(&s->b, SHOWKEY_READY);
    press(s, keys);
    press(s, "ctrl-d");
    net_wait(&s->b, last);
    net_wait(&s->b, BYTE_EOT);
    net_wait(&s->b, PROMPT);
}

/**
 * Turn Caps Lock on and off, each followed by A to show it taken, and judge
 * its LED as KDGETLED and QEMU see it.
 */
static void leds(struct kb_case *s)
{
    char *on = NULL, *off = NULL, *trace;
    const char *lit;
    int seen;

    net_step(&s->u, "leds");
    settle(s, "caps_lock a", BYTE_CAPS_A);
    net_command(&s->b, "setleds; echo LEDS\"\"DONE", "LEDSDONE", &on);
    settle(s, "caps_lock a", BYTE_A);
    net_command(&s->b, "setleds; echo LEDS\"\"DONE", "LEDSDONE", &off);
    lit = on != NULL ? strstr(on, "Current leds:") : NULL;
    net_assert(&s->u, "leds-read",
               lit != NULL && has(lit, "CapsLock on") && has(off, "Current leds:") &&
                   has(strstr(off, "Current leds:"), "CapsLock off"),
               "setleds did not read Caps Lock's LED on and then off: %.200s",
               lit != NULL ? lit : "");
    free(on);
    free(off);
    if (s->usb) {
        seen = reported(s->seen, 0x02, 0x00);
    } else {
        trace = net_file(s->seen);
        lit = trace != NULL ? strstr(trace, "ledstate 4") : NULL;
        seen = lit != NULL && has(lit, "ledstate 0");
        free(trace);
    }
    net_assert(&s->u, "leds-seen", net_ok(&s->u) && seen,
               s->usb ? "QEMU's capture has no SET_REPORT of 02 and then 00 for the keyboard"
                      : "QEMU's trace has no ps2_set_ledstate of 4 and then 0");
}

/** Run showkey in its key code mode for one key, and kbdrate twice. */
static void tools(struct kb_case *s)
{
    char *text = NULL;
    uint64_t from;

    net_step(&s->u, "tools");
    net_line(&s->b, "showkey -t 2; echo SHOWKEY=$? SHOW\"\"DONE");
    from = s->b.at;
    net_wait(&s->b, "press any key");
    press(s, "b");
    net_wait(&s->b, "SHOWDONE");
    text = net_text(&s->b, from, s->b.at);
    net_assert(&s->u, "showkey",
               has(text, "kb mode was UNICODE") && has(text, "keycode  48 press") &&
                   has(text, "keycode  48 release") && has(text, "SHOWKEY=0"),
               "showkey did not report B's press and release and exit 0: %.200s",
               text != NULL ? text : "");
    free(text);
    net_command(&s->b, "kbdrate -d 500 -r 20; kbdrate -d 250 -r 30; echo RATE=$? RATE\"\"DONE",
                "RATEDONE", &text);
    net_assert(&s->u, "kbdrate",
               has(text, "Typematic Rate set to 20.0 cps (delay = 500 ms)") &&
                   has(text, "old delay 500, period 50") && has(text, "RATE=0"),
               "kbdrate did not set and read back the repeat: %.200s", text != NULL ? text : "");
    free(text);
}

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);
    struct kb_case s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    if (strcmp(id, "machine.keyboard.ps2") == 0) {
        s.usb = 0;
    } else if (strcmp(id, "machine.keyboard.usb") == 0) {
        s.usb = 1;
    } else {
        fprintf(stderr, "keyboard_scenario: no case %s\n", id);
        return 1;
    }
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    boot(&s);
    load(&s);
    keys(&s);
    leds(&s);
    tools(&s);
    return net_end(&s.u);
}
