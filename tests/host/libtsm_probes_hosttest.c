/*
 * libtsm_probes_hosttest.c: libtsm's answers to probes, its modes 1004 and
 * 2026, OSC 52 and its strings, from 0021-probes-modes-strings.patch of
 * vendor/libtsm/patches.
 *
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-screen.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-unicode.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte-charsets.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-selection.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-render.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/shared/shl-htable.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.c
 * host-cflags: -std=gnu99 -D_GNU_SOURCE -Dwcwidth=tsm_wcwidth
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/tsm
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/shared
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external/wcwidth
 * host-cflags: -include vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.h
 *
 * Each row writes sequences to a vte over a 80x24 screen and compares the
 * bytes the vte sends back, what its callbacks receive and the cells it
 * leaves with the expected ones. A row that names a split gives the vte its
 * input in pieces, down to a byte at a time, as reads from a pty may.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libtsm.h"

static int failures;

static struct tsm_screen *screen;
static struct tsm_vte *vte;
static char out[512];
static size_t out_len;

/* What the clipboard and OSC callbacks received last, and how often. */
static int clip_calls;
static char clip_targets[16];
static char *clip_payload;
static size_t clip_len;
static int osc_calls;
static char osc_last[64];
static int led_calls;
static unsigned int led_last;

/** The vte's write callback: append what it sends to `out`. */
static void on_write(struct tsm_vte *v, const char *u8, size_t len, void *data)
{
    (void)v;
    (void)data;
    if (out_len + len > sizeof(out)) {
        fprintf(stderr, "libtsm_probes_hosttest: fail: reply overflows the buffer\n");
        exit(1);
    }
    memcpy(out + out_len, u8, len);
    out_len += len;
}

/** The clipboard callback: keep a copy of the targets and the payload. */
static void on_clipboard(struct tsm_vte *v, const char *targets, const char *payload, size_t len,
                         void *data)
{
    (void)v;
    (void)data;
    clip_calls++;
    snprintf(clip_targets, sizeof(clip_targets), "%s", targets);
    free(clip_payload);
    clip_payload = malloc(len + 1);
    if (!clip_payload) {
        fprintf(stderr, "libtsm_probes_hosttest: fail: out of memory\n");
        exit(1);
    }
    memcpy(clip_payload, payload, len);
    clip_payload[len] = '\0';
    clip_len = len;
}

/** The OSC callback: keep the start of the string. */
static void on_osc(struct tsm_vte *v, const char *u8, size_t len, void *data)
{
    (void)v;
    (void)data;
    osc_calls++;
    snprintf(osc_last, sizeof(osc_last), "%.*s", (int)(len < 63 ? len : 63), u8);
}

/** The LED callback of DECLL. */
static void on_led(struct tsm_vte *v, unsigned int leds, void *data)
{
    (void)v;
    (void)data;
    led_calls++;
    led_last = leds;
}

/** Write `s` to `buf` with ESC as \e and other controls as \xHH. */
static void quote(char *buf, size_t size, const char *s, size_t len)
{
    size_t i, n = 0;

    for (i = 0; i < len && n + 5 < size; ++i) {
        unsigned char c = (unsigned char)s[i];

        if (c == 0x1b)
            n += (size_t)snprintf(buf + n, size - n, "\\e");
        else if (c < 0x20 || c >= 0x7f)
            n += (size_t)snprintf(buf + n, size - n, "\\x%02x", c);
        else
            buf[n++] = (char)c;
    }
    buf[n] = '\0';
}

/** Compare what the vte sent since the last row with `want`, and clear it. */
static void expect(const char *what, const char *want)
{
    char got_q[600], want_q[600];
    size_t want_len = strlen(want);

    if (out_len != want_len || memcmp(out, want, want_len) != 0) {
        quote(got_q, sizeof(got_q), out, out_len);
        quote(want_q, sizeof(want_q), want, want_len);
        fprintf(stderr, "libtsm_probes_hosttest: fail: %s: sent \"%s\", want \"%s\"\n", what, got_q,
                want_q);
        failures++;
    }
    out_len = 0;
}

static void check(bool cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "libtsm_probes_hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Make a new screen and vte with every callback, as a terminal starts. */
static void start(void)
{
    if (vte)
        tsm_vte_unref(vte);
    if (screen)
        tsm_screen_unref(screen);
    screen = NULL;
    vte = NULL;
    if (tsm_screen_new(&screen, NULL, NULL) < 0 || tsm_screen_resize(screen, 80, 24) < 0 ||
        tsm_vte_new(&vte, screen, on_write, NULL, NULL, NULL) < 0) {
        fprintf(stderr, "libtsm_probes_hosttest: fail: cannot make a screen and a vte\n");
        exit(1);
    }
    tsm_vte_set_clipboard_cb(vte, on_clipboard, NULL);
    tsm_vte_set_osc_cb(vte, on_osc, NULL);
    tsm_vte_set_led_cb(vte, on_led, NULL);
    out_len = 0;
    clip_calls = 0;
    clip_targets[0] = '\0';
    free(clip_payload);
    clip_payload = NULL;
    clip_len = 0;
    osc_calls = 0;
    osc_last[0] = '\0';
    led_calls = 0;
    led_last = 0;
}

/** Write `len` bytes of the program's output to the vte, `piece` at a time. */
static void put_split(const char *s, size_t len, size_t piece)
{
    size_t i, n;

    for (i = 0; i < len; i += n) {
        n = len - i < piece ? len - i : piece;
        tsm_vte_input(vte, s + i, n);
    }
}

static void put(const char *s)
{
    tsm_vte_input(vte, s, strlen(s));
}

/* The code point drawn at a cell, found by drawing the screen. */
static unsigned int want_x, want_y;
static uint32_t found;

static int on_draw(struct tsm_screen *con, uint64_t id, const uint32_t *ch, size_t len,
                   unsigned int width, unsigned int posx, unsigned int posy,
                   const struct tsm_screen_attr *attr, tsm_age_t age, void *data)
{
    (void)con;
    (void)id;
    (void)width;
    (void)attr;
    (void)age;
    (void)data;
    if (posx == want_x && posy == want_y)
        found = len ? ch[0] : 0;
    return 0;
}

static uint32_t cell(unsigned int x, unsigned int y)
{
    want_x = x;
    want_y = y;
    found = 0;
    tsm_screen_draw(screen, on_draw, NULL);
    return found;
}

/** The first patch's calls: mode 2026's hold and mode 1004's reports. */
static void test_calls(void)
{
    start();
    check(!tsm_vte_get_sync_hold(vte), "no hold at start");
    put("\033[?2026h");
    check(tsm_vte_get_sync_hold(vte), "CSI ? 2026 h holds");
    put("\033[?2026l");
    check(!tsm_vte_get_sync_hold(vte), "CSI ? 2026 l ends the hold");
    put("\033[?2026h");
    tsm_vte_end_sync_hold(vte);
    check(!tsm_vte_get_sync_hold(vte), "tsm_vte_end_sync_hold ends the hold");
    put("\033[?2026$p");
    expect("DECRQM after the renderer ended the hold", "\033[?2026;2$y");
    put("\033[?2026h\033c");
    check(!tsm_vte_get_sync_hold(vte), "RIS ends the hold");
    put("\033[?2026h\033[!p");
    check(!tsm_vte_get_sync_hold(vte), "DECSTR ends the hold");
    expect("the hold sends nothing", "");

    check(!tsm_vte_handle_focus(vte, true), "no focus report without mode 1004");
    expect("no focus report without mode 1004", "");
    put("\033[?1004h");
    check(tsm_vte_handle_focus(vte, true), "focus in is reported");
    expect("focus in", "\033[I");
    check(tsm_vte_handle_focus(vte, false), "focus out is reported");
    expect("focus out", "\033[O");
    put("\033[?1004l");
    check(!tsm_vte_handle_focus(vte, true), "no focus report after CSI ? 1004 l");
    expect("no focus report after CSI ? 1004 l", "");
    put("\033[?1004h\033c");
    check(!tsm_vte_handle_focus(vte, false), "no focus report after RIS");
    expect("no focus report after RIS", "");
}

/** DA1, DA2, DSR, CPR, DECXCPR, OSC 10 and 11, XTVERSION and DECLL. */
static void test_probes(void)
{
    char want[64];
    struct tsm_screen_attr def;

    start();
    put("\033[c");
    expect("DA1", "\033[?62;22c");
    put("\033[0c");
    expect("DA1 with 0", "\033[?62;22c");
    put("\033Z");
    expect("DECID", "\033[?62;22c");
    put("\033[>c");
    expect("DA2", "\033[>1;1;0c");
    put("\033[5n");
    expect("DSR 5", "\033[0n");
    put("\033[?5n");
    expect("CSI ? 5 n is not DSR 5", "");

    put("\033[6n");
    expect("CPR at home", "\033[1;1R");
    put("\033[5;10H\033[6n");
    expect("CPR", "\033[5;10R");
    put("\033[?6n");
    expect("DECXCPR", "\033[?5;10R");
    put("\033[>6n");
    expect("CSI > 6 n is not CPR", "");
    put("\033[1;79HXY\033[6n");
    expect("CPR waiting to wrap", "\033[1;80R");
    put("\033[3;20r\033[?6h\033[2;4H\033[6n");
    expect("CPR in origin mode", "\033[2;4R");
    put("\033[?6l\033[2;4H\033[6n");
    expect("CPR out of origin mode", "\033[2;4R");

    tsm_vte_get_def_attr(vte, &def);
    put("\033]10;?\007");
    snprintf(want, sizeof(want), "\033]10;rgb:%02x%02x/%02x%02x/%02x%02x\007", def.fr, def.fr,
             def.fg, def.fg, def.fb, def.fb);
    expect("OSC 10 with BEL", want);
    put("\033]11;?\033\\");
    snprintf(want, sizeof(want), "\033]11;rgb:%02x%02x/%02x%02x/%02x%02x\033\\", def.br, def.br,
             def.bg, def.bg, def.bb, def.bb);
    expect("OSC 11 with ST", want);
    put("\033]10;?x\007");
    expect("OSC 10 with more than the query", "");

    put("\033[>q");
    expect("XTVERSION", "\033P>|libtsm(" TSM_VERSION ")\033\\");
    put("\033[>0q");
    expect("XTVERSION with 0", "\033P>|libtsm(" TSM_VERSION ")\033\\");
    put("\033[>1q");
    expect("CSI > 1 q is not XTVERSION", "");
    check(led_calls == 0, "XTVERSION does not set the LEDs");
    put("\033[2q");
    check(led_calls == 1 && led_last == TSM_VTE_LED_NUM_LOCK, "DECLL sets Num Lock's LED");
    put("\033[\"q\033[?2q");
    check(led_calls == 1, "CSI \" q and CSI ? q are not DECLL");
    put("\033[0q");
    check(led_calls == 2 && led_last == 0, "DECLL 0 clears the LEDs");
}

/** Send DECRQM for `mode` and compare its answer. */
static void rqm(bool dec, int mode, int state)
{
    char req[32], want[32], what[64];

    snprintf(req, sizeof(req), "\033[%s%d$p", dec ? "?" : "", mode);
    snprintf(want, sizeof(want), "\033[%s%d;%d$y", dec ? "?" : "", mode, state);
    snprintf(what, sizeof(what), "DECRQM %s%d", dec ? "?" : "", mode);
    put(req);
    expect(what, want);
}

/** DECRQM for every mode the parser takes, set and reset. */
static void test_decrqm(void)
{
    static const int dec_modes[] = {1,    5,    6,    7,    8,    9,    25,   42,   47,  1000,
                                    1002, 1003, 1004, 1006, 1016, 1047, 1049, 2004, 2026};
    static const int ansi_modes[] = {2, 4, 12, 20};
    char buf[32];
    size_t i;

    start();
    rqm(false, 4, 2);
    rqm(false, 12, 1);
    rqm(false, 20, 2);
    rqm(false, 3, 0);
    for (i = 0; i < sizeof(ansi_modes) / sizeof(ansi_modes[0]); ++i) {
        snprintf(buf, sizeof(buf), "\033[%dh", ansi_modes[i]);
        put(buf);
        rqm(false, ansi_modes[i], 1);
        snprintf(buf, sizeof(buf), "\033[%dl", ansi_modes[i]);
        put(buf);
        rqm(false, ansi_modes[i], 2);
    }

    start();
    rqm(true, 1, 2);
    rqm(true, 7, 1);
    rqm(true, 25, 1);
    rqm(true, 2026, 2);
    rqm(true, 2, 3);
    rqm(true, 3, 4);
    rqm(true, 4, 4);
    rqm(true, 18, 4);
    rqm(true, 19, 4);
    rqm(true, 12, 0);
    rqm(true, 2027, 0);
    rqm(true, 9999, 0);
    rqm(true, 1048, 2);
    put("\033[?1048h");
    rqm(true, 1048, 1);
    for (i = 0; i < sizeof(dec_modes) / sizeof(dec_modes[0]); ++i) {
        start();
        snprintf(buf, sizeof(buf), "\033[?%dh", dec_modes[i]);
        put(buf);
        rqm(true, dec_modes[i], 1);
        snprintf(buf, sizeof(buf), "\033[?%dl", dec_modes[i]);
        put(buf);
        rqm(true, dec_modes[i], 2);
    }

    start();
    put("\033[?2027h");
    rqm(true, 2027, 0);
    put("\033[>2026$p\033[?2026 $p");
    expect("DECRQM with another marker", "");
}

/*
 * A group of probes as an application sends it, ended by DA1: the answers
 * come in the order of the probes, and those it does not answer, an unknown
 * OSC and XTGETTCAP, delay none of the others. Whole and split.
 */
static void test_order(void)
{
    static const char probes[] = "\033[?u\033[?2026$p\033[>q\033]999;?\007\033P+q544e\033\\"
                                 "\033[6n\033]10;?\033\\\033[c";
    char want[256];
    struct tsm_screen_attr def;
    size_t piece;
    int runs = 0;

    start();
    tsm_vte_get_def_attr(vte, &def);
    snprintf(want, sizeof(want),
             "\033[?0u\033[?2026;2$y\033P>|libtsm(" TSM_VERSION
             ")\033\\\033[1;1R\033]10;rgb:%02x%02x/%02x%02x/%02x%02x\033\\\033[?62;22c",
             def.fr, def.fr, def.fg, def.fg, def.fb, def.fb);
    for (piece = 1; piece <= sizeof(probes); piece *= 3) {
        put_split(probes, sizeof(probes) - 1, piece);
        expect(piece == 1 ? "probes byte by byte" : "probes in pieces", want);
        runs++;
    }
    check(osc_calls == runs, "the unknown OSC reaches the OSC callback each time");
    check(strcmp(osc_last, "999;?") == 0, "the OSC callback has the unknown OSC");
}

/** OSC 52 to the clipboard callback: whole, split, cancelled and without it. */
static void test_osc52(void)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t big = 1398104; /* 1 MiB of text in base64 */
    char *s;
    size_t i, n;

    start();
    put("\033]52;c;aGVsbG8=\007");
    check(clip_calls == 1 && strcmp(clip_targets, "c") == 0 && clip_len == 8 &&
              strcmp(clip_payload, "aGVsbG8=") == 0,
          "OSC 52 with BEL");
    put("\033]52;;d29ybGQ=\033\\");
    check(clip_calls == 2 && clip_targets[0] == '\0' && strcmp(clip_payload, "d29ybGQ=") == 0,
          "OSC 52 with ST and no targets");
    put("\033]52;p;?\007");
    check(clip_calls == 3 && strcmp(clip_targets, "p") == 0 && strcmp(clip_payload, "?") == 0,
          "OSC 52's query reaches the callback");
    put("\033]52;c;\007");
    check(clip_calls == 4 && clip_len == 0, "OSC 52 with an empty payload");
    put("\033]52;c\007");
    check(clip_calls == 4, "OSC 52 without a payload is dropped");
    put("\033]52;c;aGk=\030");
    check(clip_calls == 4, "OSC 52 cancelled by CAN");
    put("\033]52;c;aGk=\302\205");
    check(clip_calls == 4, "OSC 52 cancelled by NEL");
    expect("OSC 52 sends nothing", "");
    check(osc_calls == 0, "OSC 52 does not reach the OSC callback");
    check(tsm_screen_get_cursor_x(screen) == 0, "OSC 52 prints nothing");

    s = malloc(big + 32);
    if (!s) {
        fprintf(stderr, "libtsm_probes_hosttest: fail: out of memory\n");
        exit(1);
    }
    n = (size_t)sprintf(s, "\033]52;c;");
    for (i = 0; i < big; ++i)
        s[n + i] = b64[i * 7 % 64];
    n += big;
    s[n++] = '\007';
    put_split(s, n, 4096);
    check(clip_calls == 5 && clip_len == big && clip_payload[0] == 'A' &&
              clip_payload[big - 1] == b64[(big - 1) * 7 % 64],
          "1 MiB of text in base64, in reads of 4096 bytes");
    put_split("\033]52;c;aGk=\007", 12, 1);
    check(clip_calls == 6 && strcmp(clip_payload, "aGk=") == 0, "OSC 52 byte by byte");
    free(s);

    tsm_vte_set_clipboard_cb(vte, NULL, NULL);
    put("\033]52;c;aGk=\007");
    check(clip_calls == 6 && osc_calls == 1 && strcmp(osc_last, "52;c;aGk=") == 0,
          "OSC 52 without a clipboard callback reaches the OSC callback");
}

/*
 * The introducers and terminators of the strings: OSC, DCS, SOS, PM and APC
 * with ST, and OSC with BEL too.
 */
static const struct {
    const char *name;
    const char *open;
    const char *close;
} strings[] = {
    {"OSC", "\033]", "\033\\"},         {"OSC with BEL", "\033]", "\007"},
    {"DCS", "\033P", "\033\\"},         {"SOS", "\033X", "\033\\"},
    {"PM", "\033^", "\033\\"},          {"APC", "\033_", "\033\\"},
    {"C1 OSC", "\302\235", "\302\234"}, {"C1 DCS", "\302\220", "\302\234"},
    {"C1 APC", "\302\237", "\302\234"},
};

/** Send `body` in string kind `k`, then "Z", and check that only "Z" shows. */
static void consumed(size_t k, const char *body, size_t len, size_t piece, const char *what)
{
    char buf[128];

    start();
    put_split(strings[k].open, strlen(strings[k].open), 1);
    put_split(body, len, piece);
    put_split(strings[k].close, strlen(strings[k].close), 1);
    put("Z");
    snprintf(buf, sizeof(buf), "%s %s", strings[k].name, what);
    expect(buf, "");
    snprintf(buf, sizeof(buf), "%s %s prints nothing", strings[k].name, what);
    check(tsm_screen_get_cursor_x(screen) == 1 && tsm_screen_get_cursor_y(screen) == 0 &&
              cell(0, 0) == 'Z',
          buf);
    snprintf(buf, sizeof(buf), "%s %s calls nothing", strings[k].name, what);
    check(clip_calls == 0 && led_calls == 0 && (k <= 1 || k == 6 ? osc_calls <= 1 : !osc_calls),
          buf);
}

/** Unknown and oversized strings, whole and split, consumed without effect. */
static void test_strings(void)
{
    static const char unknown[] = "+q544e;1;2$q\"p[31mtitle\303\251\007more";
    size_t big = 3 * 1024 * 1024;
    char *body;
    size_t k, i;

    body = malloc(big);
    if (!body) {
        fprintf(stderr, "libtsm_probes_hosttest: fail: out of memory\n");
        exit(1);
    }
    for (i = 0; i < big; ++i)
        body[i] = (char)('A' + i % 26);
    memcpy(body, "52;c;", 5);

    for (k = 0; k < sizeof(strings) / sizeof(strings[0]); ++k) {
        /* The unknown body holds BEL, which ends an OSC early. */
        if (k > 1 && k != 6) {
            consumed(k, unknown, sizeof(unknown) - 1, sizeof(unknown), "unknown");
            consumed(k, unknown, sizeof(unknown) - 1, 1, "unknown byte by byte");
        } else {
            consumed(k, "999;x", 5, 5, "unknown");
            consumed(k, "999;x", 5, 1, "unknown byte by byte");
        }
        consumed(k, body, big, 65536, "of 3 MiB");
        if (k <= 1 || k == 6)
            check(osc_calls == 0, "an oversized OSC is dropped");
    }

    /* A string after an oversized one is taken whole. */
    start();
    put_split("\033]0;", 4, 4);
    put_split(body, big, 65536);
    put("\007\033]52;c;aGk=\007");
    check(clip_calls == 1 && strcmp(clip_payload, "aGk=") == 0, "OSC 52 after an oversized OSC");
    put("\033]2;title\007");
    check(osc_calls == 1 && strcmp(osc_last, "2;title") == 0, "a title after an oversized OSC");

    /* A UTF-8 title split inside a character. */
    put_split("\033]2;\320\232\320\276\007", 9, 1);
    check(osc_calls == 2 && strcmp(osc_last, "2;\320\232\320\276") == 0,
          "a UTF-8 title byte by byte");
    free(body);
}

int main(void)
{
    test_calls();
    test_probes();
    test_decrqm();
    test_order();
    test_osc52();
    test_strings();

    if (vte)
        tsm_vte_unref(vte);
    if (screen)
        tsm_screen_unref(screen);
    free(clip_payload);

    if (failures) {
        fprintf(stderr, "libtsm_probes_hosttest: %d failures\n", failures);
        return 1;
    }
    printf("libtsm_probes_hosttest: pass\n");
    return 0;
}
