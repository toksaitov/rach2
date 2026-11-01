/*
 * libtsm_keys_hosttest.c: libtsm's keyboard protocols, from the three
 * 0018- patches of vendor/libtsm/patches: the CSI collector's < and =, SCOSC
 * and SCORC, xterm's legacy key encoding, and the Kitty keyboard protocol.
 *
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-vte.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-screen.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-unicode.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-vte-charsets.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-selection.c
 * host-deps: build/host/libtsm-4.8.0/src/tsm/tsm-render.c
 * host-deps: build/host/libtsm-4.8.0/src/shared/shl-htable.c
 * host-deps: build/host/libtsm-4.8.0/external/wcwidth/wcwidth.c
 * host-cflags: -std=gnu99 -D_GNU_SOURCE -Dwcwidth=tsm_wcwidth
 * host-cflags: -I build/host/libtsm-4.8.0/src/tsm
 * host-cflags: -I build/host/libtsm-4.8.0/src/shared
 * host-cflags: -I build/host/libtsm-4.8.0/external
 * host-cflags: -I build/host/libtsm-4.8.0/external/wcwidth
 * host-cflags: -include build/host/libtsm-4.8.0/external/wcwidth/wcwidth.h
 *
 * Each row writes sequences to a vte over a 80x24 screen, or gives it a key,
 * and compares the bytes the vte sends back with the bytes expected. The
 * Kitty rows include the protocol's own examples from its specification,
 * https://sw.kovidgoyal.net/kitty/keyboard-protocol/: ctrl+shift+a is
 * CSI 97;6u and never CSI 65;6u, ctrl+shift+tab is CSI 9;6u, Russian
 * ctrl+C is CSI 1089::99;5u with alternate keys, the forms of the press,
 * repeat and release events, and the legacy text keys i, 3 and ; with Ctrl
 * and Shift. The legacy rows are xterm's bytes from tests/host/keytable.txt
 * for the keys whose keysyms libtsm sees.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "libtsm.h"

static int failures;

static struct tsm_screen *screen;
static struct tsm_vte *vte;
static char out[512];
static size_t out_len;

/** The vte's write callback: append what it sends to `out`. */
static void on_write(struct tsm_vte *v, const char *u8, size_t len, void *data)
{
    (void)v;
    (void)data;
    if (out_len + len > sizeof(out)) {
        fprintf(stderr, "libtsm_keys_hosttest: fail: reply overflows the buffer\n");
        exit(1);
    }
    memcpy(out + out_len, u8, len);
    out_len += len;
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
static void expect_len(const char *what, const char *want, size_t want_len)
{
    char got_q[256], want_q[256];

    if (out_len != want_len || memcmp(out, want, want_len) != 0) {
        quote(got_q, sizeof(got_q), out, out_len);
        quote(want_q, sizeof(want_q), want, want_len);
        fprintf(stderr, "libtsm_keys_hosttest: fail: %s: sent \"%s\", want \"%s\"\n", what, got_q,
                want_q);
        failures++;
    }
    out_len = 0;
}

static void expect(const char *what, const char *want)
{
    expect_len(what, want, strlen(want));
}

static void check(bool cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "libtsm_keys_hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Make a new screen and vte, as a terminal starts. */
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
        fprintf(stderr, "libtsm_keys_hosttest: fail: cannot make a screen and a vte\n");
        exit(1);
    }
    tsm_vte_set_backspace_sends_delete(vte, true);
    out_len = 0;
}

/** Write the application's output `s` to the vte. */
static void put(const char *s)
{
    tsm_vte_input(vte, s, strlen(s));
}

/** Press a key and compare what it sends. */
static void key(const char *what, uint32_t keysym, uint32_t ascii, unsigned int mods,
                uint32_t unicode, const char *want)
{
    tsm_vte_handle_keyboard(vte, keysym, ascii, mods, unicode);
    expect(what, want);
}

/** Give a key event with the Kitty protocol's alternate keys. */
static void kkey(const char *what, uint32_t keysym, unsigned int mods, uint32_t unicode,
                 uint32_t unshifted, uint32_t base, unsigned int event, const char *want)
{
    uint32_t ascii = base < 0x80 ? base : XKB_KEY_NoSymbol;

    tsm_vte_handle_key(vte, keysym, ascii, mods, unicode, unshifted, base, event);
    expect(what, want);
}

#define S       TSM_SHIFT_MASK
#define C       TSM_CONTROL_MASK
#define A       TSM_ALT_MASK
#define CAPS    TSM_LOCK_MASK
#define NUM     TSM_NUM_LOCK_MASK
#define INV     TSM_VTE_INVALID
#define PRESS   TSM_VTE_KEY_PRESS
#define REPEAT  TSM_VTE_KEY_REPEAT
#define RELEASE TSM_VTE_KEY_RELEASE

/** The CSI collector's < and =, and the Kitty protocol's requests. */
static void test_requests(void)
{
    int i;
    char buf[32];

    start();
    put("\033[?u");
    expect("query with no flags", "\033[?0u");
    put("\033[>1u\033[?u");
    expect("push 1", "\033[?1u");
    check(tsm_vte_get_key_flags(vte) == 1, "tsm_vte_get_key_flags gives the pushed flags");
    put("\033[>3u\033[?u");
    expect("push 3", "\033[?3u");
    put("\033[<u\033[?u");
    expect("pop one", "\033[?1u");
    put("\033[<u\033[?u");
    expect("pop to the bottom", "\033[?0u");
    put("\033[<u\033[?u");
    expect("pop past the bottom", "\033[?0u");

    put("\033[>1u\033[>2u\033[>4u\033[<2u\033[?u");
    expect("pop two", "\033[?1u");
    put("\033[<9u\033[?u");
    expect("pop more than pushed", "\033[?0u");

    /* Nine pushes in a stack of eight: the oldest is lost. */
    for (i = 1; i <= 9; ++i) {
        snprintf(buf, sizeof(buf), "\033[>%du", (i - 1) % 7 + 1);
        put(buf);
    }
    put("\033[?u");
    expect("ninth push", "\033[?2u");
    put("\033[<u\033[?u");
    expect("pop after nine pushes", "\033[?1u");
    put("\033[<7u\033[?u");
    expect("pop the stack's depth", "\033[?0u");

    put("\033[=31u\033[?u");
    expect("set drops the unsupported flags", "\033[?7u");
    put("\033[=2;3u\033[?u");
    expect("set mode 3 clears", "\033[?5u");
    put("\033[=8;2u\033[?u");
    expect("set mode 2 of an unsupported flag", "\033[?5u");
    put("\033[=2;2u\033[?u");
    expect("set mode 2 sets", "\033[?7u");
    put("\033[=1u\033[?u");
    expect("set mode 1 replaces", "\033[?1u");
    put("\033[=u\033[?u");
    expect("set with no flags", "\033[?0u");
    put("\033[>24u\033[?u");
    expect("push drops the unsupported flags", "\033[?0u");
    put("\033[<u");

    /* The main and the alternate screen keep stacks of their own. */
    put("\033[>1u\033[?1049h\033[?u");
    expect("the alternate screen's stack is empty", "\033[?0u");
    put("\033[>3u\033[?u");
    expect("push on the alternate screen", "\033[?3u");
    put("\033[?1049l\033[?u");
    expect("the main screen's stack is kept", "\033[?1u");
    put("\033[?1049h\033[?u");
    expect("the alternate screen's stack is kept", "\033[?3u");
    put("\033[?1049l");

    put("\033c\033[?u");
    expect("RIS empties the stacks", "\033[?0u");
    put("\033[?1049h\033[?u");
    expect("RIS empties the alternate screen's stack", "\033[?0u");
    put("\033[?1049l");

    /* A request split across writes. */
    put("\033");
    put("[");
    put("?");
    put("u");
    expect("query split byte by byte", "\033[?0u");

    /* = and < no longer reach the plain sequence of the same final. */
    put("\033[=c");
    expect("CSI = c is not DA1", "");
    put("\033[c");
    check(out_len > 0, "CSI c still answers DA1");
    out_len = 0;
}

/** Plain CSI s and CSI u save and restore the cursor; private forms do not. */
static void test_scosc(void)
{
    start();
    put("\033[4;6H\033[s\033[1;1H\033[u");
    check(tsm_screen_get_cursor_x(screen) == 5 && tsm_screen_get_cursor_y(screen) == 3,
          "CSI s and CSI u save and restore the cursor");

    put("\033[10;10H\033[s\033[2;2H\033[?s\033[u");
    check(tsm_screen_get_cursor_x(screen) == 9 && tsm_screen_get_cursor_y(screen) == 9,
          "CSI ? s does not save the cursor");

    put("\033[5;5H\033[?u\033[>1u\033[=1u\033[<u");
    expect("the keyboard requests", "\033[?0u");
    check(tsm_screen_get_cursor_x(screen) == 4 && tsm_screen_get_cursor_y(screen) == 4,
          "the keyboard protocol's CSI u forms do not restore the cursor");

    put("\033[3;3H\033[2u");
    check(tsm_screen_get_cursor_x(screen) == 2 && tsm_screen_get_cursor_y(screen) == 2,
          "CSI 2 u is not SCORC");

    put("\033[6;7H\0337\033[1;1H\033[u");
    check(tsm_screen_get_cursor_x(screen) == 6 && tsm_screen_get_cursor_y(screen) == 5,
          "CSI u restores what DECSC saved");
}

/* xterm's modifier columns of keytable.txt: none, Shift, Ctrl, Ctrl+Shift,
 * Alt, Alt+Shift, Ctrl+Alt, Ctrl+Alt+Shift, and their parameters. */
static const unsigned int col_mods[8] = {0, S, C, C | S, A, A | S, C | A, C | A | S};
static const unsigned int col_param[8] = {1, 2, 5, 6, 3, 4, 7, 8};

/** The function keys, editing keys and cursor keys under every modifier. */
static void test_legacy_functional(void)
{
    static const struct {
        uint32_t keysym;
        unsigned int num;
        char final;
        const char *plain;
        const char *ckm;
    } keys[] = {
        {XKB_KEY_F1, 1, 'P', "\033OP", "\033OP"},
        {XKB_KEY_F2, 1, 'Q', "\033OQ", "\033OQ"},
        {XKB_KEY_F3, 1, 'R', "\033OR", "\033OR"},
        {XKB_KEY_F4, 1, 'S', "\033OS", "\033OS"},
        {XKB_KEY_F5, 15, '~', "\033[15~", "\033[15~"},
        {XKB_KEY_F6, 17, '~', "\033[17~", "\033[17~"},
        {XKB_KEY_F7, 18, '~', "\033[18~", "\033[18~"},
        {XKB_KEY_F8, 19, '~', "\033[19~", "\033[19~"},
        {XKB_KEY_F9, 20, '~', "\033[20~", "\033[20~"},
        {XKB_KEY_F10, 21, '~', "\033[21~", "\033[21~"},
        {XKB_KEY_F11, 23, '~', "\033[23~", "\033[23~"},
        {XKB_KEY_F12, 24, '~', "\033[24~", "\033[24~"},
        {XKB_KEY_Insert, 2, '~', "\033[2~", "\033[2~"},
        {XKB_KEY_Delete, 3, '~', "\033[3~", "\033[3~"},
        {XKB_KEY_Page_Up, 5, '~', "\033[5~", "\033[5~"},
        {XKB_KEY_Page_Down, 6, '~', "\033[6~", "\033[6~"},
        {XKB_KEY_Menu, 29, '~', "\033[29~", "\033[29~"},
        {XKB_KEY_Up, 1, 'A', "\033[A", "\033OA"},
        {XKB_KEY_Down, 1, 'B', "\033[B", "\033OB"},
        {XKB_KEY_Right, 1, 'C', "\033[C", "\033OC"},
        {XKB_KEY_Left, 1, 'D', "\033[D", "\033OD"},
        {XKB_KEY_Home, 1, 'H', "\033[H", "\033OH"},
        {XKB_KEY_End, 1, 'F', "\033[F", "\033OF"},
        {XKB_KEY_KP_Home, 1, 'H', "\033[H", "\033OH"},
        {XKB_KEY_KP_Up, 1, 'A', "\033[A", "\033OA"},
        {XKB_KEY_KP_Begin, 1, 'E', "\033[E", "\033OE"},
        {XKB_KEY_KP_Page_Up, 5, '~', "\033[5~", "\033[5~"},
        {XKB_KEY_KP_Insert, 2, '~', "\033[2~", "\033[2~"},
        {XKB_KEY_KP_Delete, 3, '~', "\033[3~", "\033[3~"},
    };
    char want[32], what[64];
    size_t k;
    int c, ckm;

    for (ckm = 0; ckm < 2; ++ckm) {
        start();
        if (ckm)
            put("\033[?1h");
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
            for (c = 0; c < 8; ++c) {
                if (c == 0)
                    snprintf(want, sizeof(want), "%s", ckm ? keys[k].ckm : keys[k].plain);
                else
                    snprintf(want, sizeof(want), "\033[%u;%u%c", keys[k].num, col_param[c],
                             keys[k].final);
                snprintf(what, sizeof(what), "keysym 0x%x, column %d%s", (unsigned)keys[k].keysym,
                         c, ckm ? ", DECCKM" : "");
                key(what, keys[k].keysym, XKB_KEY_NoSymbol, col_mods[c], INV, want);
            }
        }
    }

    start();
    key("Shift+F1 is not F11", XKB_KEY_F1, XKB_KEY_NoSymbol, S, INV, "\033[1;2P");
    key("Shift+F5 is not F15", XKB_KEY_F5, XKB_KEY_NoSymbol, S, INV, "\033[15;2~");
    key("Sys_Req sends nothing", XKB_KEY_Sys_Req, XKB_KEY_NoSymbol, 0, INV, "");
    check(!tsm_vte_handle_keyboard(vte, XKB_KEY_Sys_Req, XKB_KEY_NoSymbol, 0, INV),
          "Sys_Req is not handled");
    check(!tsm_vte_handle_keyboard(vte, XKB_KEY_Shift_L, XKB_KEY_NoSymbol, 0, INV),
          "Shift alone is not handled");
    expect("modifiers alone send nothing", "");
}

/** Escape, Tab, Enter, Backspace and Space under every modifier. */
static void test_legacy_c0(void)
{
    static const struct {
        uint32_t keysym;
        uint32_t ascii;
        uint32_t unicode;
        const char *cells[8];
    } keys[] = {
        {XKB_KEY_Escape,
         XKB_KEY_NoSymbol,
         INV,
         {"\033", "\033", "\033", "\033", "\033\033", "\033\033", "\033\033", "\033\033"}},
        {XKB_KEY_Tab,
         XKB_KEY_NoSymbol,
         INV,
         {"\t", "\033[Z", "\t", "\033[Z", "\033\t", "\033[Z", "\033\t", "\033[Z"}},
        {XKB_KEY_Return,
         XKB_KEY_NoSymbol,
         INV,
         {"\r", "\r", "\r", "\r", "\033\r", "\033\r", "\033\r", "\033\r"}},
        {XKB_KEY_BackSpace,
         XKB_KEY_NoSymbol,
         INV,
         {"\x7f", "\x7f", "\b", "\b", "\033\x7f", "\033\x7f", "\033\b", "\033\b"}},
    };
    char what[64];
    size_t k;
    int c;

    start();
    for (k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
        for (c = 0; c < 8; ++c) {
            snprintf(what, sizeof(what), "keysym 0x%x, column %d", (unsigned)keys[k].keysym, c);
            key(what, keys[k].keysym, keys[k].ascii, col_mods[c], keys[k].unicode,
                keys[k].cells[c]);
        }
    }
    key("ISO_Left_Tab", XKB_KEY_ISO_Left_Tab, XKB_KEY_NoSymbol, S, INV, "\033[Z");
    key("Alt+ISO_Left_Tab", XKB_KEY_ISO_Left_Tab, XKB_KEY_NoSymbol, S | A, INV, "\033[Z");

    tsm_vte_set_backspace_sends_delete(vte, false);
    key("Backspace sending BS", XKB_KEY_BackSpace, XKB_KEY_NoSymbol, 0, INV, "\b");
    key("Ctrl+Backspace sending DEL", XKB_KEY_BackSpace, XKB_KEY_NoSymbol, C, INV, "\x7f");

    put("\033[20h");
    key("Enter in LNM", XKB_KEY_Return, XKB_KEY_NoSymbol, 0, INV, "\r\n");
}

/** The text keys of keytable.txt's first block with Ctrl, and Ctrl in
 * another layout through `ascii`. */
static void test_legacy_ctrl(void)
{
    static const struct {
        char base, shifted;
        const char *ctrl;
        const char *ctrl_shift;
    } keys[] = {
        {'`', '~', "\x00", "\x1e"}, {'1', '!', "1", "!"},       {'2', '@', "\x00", "\x00"},
        {'3', '#', "\033", "#"},    {'4', '$', "\x1c", "$"},    {'5', '%', "\x1d", "%"},
        {'6', '^', "\x1e", "\x1e"}, {'7', '&', "\x1f", "&"},    {'8', '*', "\x7f", "*"},
        {'9', '(', "9", "("},       {'0', ')', "0", ")"},       {'-', '_', "-", "\x1f"},
        {'=', '+', "=", "+"},       {'q', 'Q', "\x11", "\x11"}, {'i', 'I', "\t", "\t"},
        {'[', '{', "\033", "\033"}, {']', '}', "\x1d", "\x1d"}, {'\\', '|', "\x1c", "\x1c"},
        {'a', 'A', "\x01", "\x01"}, {'h', 'H', "\b", "\b"},     {'j', 'J', "\n", "\n"},
        {';', ':', ";", ":"},       {'\'', '"', "'", "\""},     {'m', 'M', "\r", "\r"},
        {',', '<', ",", "<"},       {'.', '>', ".", ">"},       {'/', '?', "\x1f", "\x7f"},
        {' ', ' ', "\x00", "\x00"},
    };
    char what[64], want[8];
    size_t k;

    start();
    for (k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
        uint32_t b = (unsigned char)keys[k].base, s = (unsigned char)keys[k].shifted;

        snprintf(what, sizeof(what), "Ctrl+%c", keys[k].base);
        tsm_vte_handle_keyboard(vte, b, b, C, b);
        expect_len(what, keys[k].ctrl, strlen(keys[k].ctrl) ? strlen(keys[k].ctrl) : 1);

        snprintf(what, sizeof(what), "Ctrl+Shift+%c", keys[k].base);
        tsm_vte_handle_keyboard(vte, s, s, C | S, s);
        expect_len(what, keys[k].ctrl_shift,
                   strlen(keys[k].ctrl_shift) ? strlen(keys[k].ctrl_shift) : 1);

        snprintf(what, sizeof(what), "Ctrl+Alt+%c", keys[k].base);
        want[0] = '\033';
        memcpy(want + 1, keys[k].ctrl, 2);
        tsm_vte_handle_keyboard(vte, b, b, C | A, b);
        expect_len(what, want, 1 + (strlen(keys[k].ctrl) ? strlen(keys[k].ctrl) : 1));

        snprintf(what, sizeof(what), "Alt+Shift+%c", keys[k].base);
        want[1] = keys[k].shifted;
        tsm_vte_handle_keyboard(vte, s, s, A | S, s);
        expect_len(what, want, 2);
    }

    key("Ctrl+C with Caps Lock", 'C', 'C', C | CAPS, 'C', "\x03");
    key("Russian Ctrl+es", XKB_KEY_Cyrillic_es, 'c', C, 0x441, "\x03");
    key("Russian es", XKB_KEY_Cyrillic_es, 'c', 0, 0x441, "\xd1\x81");
    key("Russian Alt+es", XKB_KEY_Cyrillic_es, 'c', A, 0x441, "\033\xd1\x81");
    key("Russian Ctrl+1 keeps its text", '1', '1', C, '1', "1");
}

/** The keypad: Num Lock off and on, in numeric and application mode. */
static void test_legacy_keypad(void)
{
    static const struct {
        uint32_t keysym;
        const char *text;
        char final;
    } keys[] = {
        {XKB_KEY_KP_Divide, "/", 'o'},   {XKB_KEY_KP_Multiply, "*", 'j'},
        {XKB_KEY_KP_Subtract, "-", 'm'}, {XKB_KEY_KP_Add, "+", 'k'},
        {XKB_KEY_KP_Enter, "\r", 'M'},   {XKB_KEY_KP_Decimal, ".", 'n'},
        {XKB_KEY_KP_Equal, "=", 'X'},    {XKB_KEY_KP_Separator, ",", 'l'},
        {XKB_KEY_KP_0, "0", 'p'},        {XKB_KEY_KP_5, "5", 'u'},
        {XKB_KEY_KP_7, "7", 'w'},        {XKB_KEY_KP_9, "9", 'y'},
    };
    char what[64], want[16];
    size_t k;
    int c, kpam, num;

    for (kpam = 0; kpam < 2; ++kpam) {
        for (num = 0; num < 2; ++num) {
            start();
            put(kpam ? "\033=" : "\033>");
            for (k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
                for (c = 0; c < 8; ++c) {
                    if (kpam && !num && c == 0)
                        snprintf(want, sizeof(want), "\033O%c", keys[k].final);
                    else if (kpam && !num)
                        snprintf(want, sizeof(want), "\033O%u%c", col_param[c], keys[k].final);
                    else
                        snprintf(want, sizeof(want), "%s", keys[k].text);
                    snprintf(what, sizeof(what), "keysym 0x%x, column %d%s%s",
                             (unsigned)keys[k].keysym, c, kpam ? ", DECKPAM" : "",
                             num ? ", Num Lock" : "");
                    key(what, keys[k].keysym, XKB_KEY_NoSymbol, col_mods[c] | (num ? NUM : 0), INV,
                        want);
                }
            }
        }
    }

    start();
    put("\033=");
    key("application divide is SS3 o", XKB_KEY_KP_Divide, XKB_KEY_NoSymbol, 0, INV, "\033Oo");
    key("application multiply is SS3 j", XKB_KEY_KP_Multiply, XKB_KEY_NoSymbol, 0, INV, "\033Oj");
    key("KP_Home in DECKPAM is Home", XKB_KEY_KP_Home, XKB_KEY_NoSymbol, 0, INV, "\033[H");
    put("\033[20h\033>");
    key("KP_Enter in LNM", XKB_KEY_KP_Enter, XKB_KEY_NoSymbol, 0, INV, "\r\n");
}

/* The specification's legacy text keys, with Ctrl+Shift under flag 1. */
static void test_kitty_spec(void)
{
    start();
    put("\033[>1u");
    kkey("ctrl+shift+a is CSI 97;6u, never 65", 'A', C | S, 'A', 'a', 'a', PRESS, "\033[97;6u");
    kkey("ctrl+shift+i", 'I', C | S, 'I', 'i', 'i', PRESS, "\033[105;6u");
    kkey("ctrl+shift+3", '#', C | S, '#', '3', '3', PRESS, "\033[51;6u");
    kkey("ctrl+shift+;", ':', C | S, ':', ';', ';', PRESS, "\033[59;6u");
    kkey("ctrl+shift+tab is CSI 9;6u", XKB_KEY_ISO_Left_Tab, C | S, INV, INV, INV, PRESS,
         "\033[9;6u");
    kkey("i", 'i', 0, 'i', 'i', 'i', PRESS, "i");
    kkey("shift+i", 'I', S, 'I', 'i', 'i', PRESS, "I");
    kkey("alt+i", 'i', A, 'i', 'i', 'i', PRESS, "\033[105;3u");
    kkey("ctrl+i", 'i', C, 'i', 'i', 'i', PRESS, "\033[105;5u");
    kkey("shift+alt+i", 'I', S | A, 'I', 'i', 'i', PRESS, "\033[105;4u");
    kkey("alt+ctrl+i", 'i', A | C, 'i', 'i', 'i', PRESS, "\033[105;7u");
    kkey("ctrl+c no longer sends ETX", 'c', C, 'c', 'c', 'c', PRESS, "\033[99;5u");
    kkey("Esc is disambiguated", XKB_KEY_Escape, 0, INV, INV, INV, PRESS, "\033[27u");
    kkey("Alt+Esc", XKB_KEY_Escape, A, INV, INV, INV, PRESS, "\033[27;3u");
    kkey("Enter keeps its byte", XKB_KEY_Return, 0, INV, INV, INV, PRESS, "\r");
    kkey("Tab keeps its byte", XKB_KEY_Tab, 0, INV, INV, INV, PRESS, "\t");
    kkey("Backspace keeps its byte", XKB_KEY_BackSpace, 0, INV, INV, INV, PRESS, "\x7f");
    kkey("Shift+Enter", XKB_KEY_Return, S, INV, INV, INV, PRESS, "\033[13;2u");
    kkey("Ctrl+Enter", XKB_KEY_Return, C, INV, INV, INV, PRESS, "\033[13;5u");
    kkey("Alt+Enter", XKB_KEY_Return, A, INV, INV, INV, PRESS, "\033[13;3u");
    kkey("Shift+Tab", XKB_KEY_ISO_Left_Tab, S, INV, INV, INV, PRESS, "\033[9;2u");
    kkey("Ctrl+Backspace", XKB_KEY_BackSpace, C, INV, INV, INV, PRESS, "\033[127;5u");
    kkey("ctrl+space", ' ', C, ' ', ' ', ' ', PRESS, "\033[32;5u");
    kkey("shift+space is text", ' ', S, ' ', ' ', ' ', PRESS, " ");
    kkey("Up", XKB_KEY_Up, 0, INV, INV, INV, PRESS, "\033[A");
    kkey("Shift+Up", XKB_KEY_Up, S, INV, INV, INV, PRESS, "\033[1;2A");
    kkey("Home", XKB_KEY_Home, 0, INV, INV, INV, PRESS, "\033[H");
    kkey("Ctrl+End", XKB_KEY_End, C, INV, INV, INV, PRESS, "\033[1;5F");
    kkey("F1", XKB_KEY_F1, 0, INV, INV, INV, PRESS, "\033[P");
    kkey("Shift+F1", XKB_KEY_F1, S, INV, INV, INV, PRESS, "\033[1;2P");
    kkey("F3 is CSI 13 ~, not CSI R", XKB_KEY_F3, 0, INV, INV, INV, PRESS, "\033[13~");
    kkey("F5", XKB_KEY_F5, 0, INV, INV, INV, PRESS, "\033[15~");
    kkey("Alt+F12", XKB_KEY_F12, A, INV, INV, INV, PRESS, "\033[24;3~");
    kkey("F13", XKB_KEY_F13, 0, INV, INV, INV, PRESS, "\033[57376u");
    kkey("Insert", XKB_KEY_Insert, 0, INV, INV, INV, PRESS, "\033[2~");
    kkey("Delete", XKB_KEY_Delete, 0, INV, INV, INV, PRESS, "\033[3~");
    kkey("Page Up", XKB_KEY_Page_Up, 0, INV, INV, INV, PRESS, "\033[5~");
    kkey("Pause", XKB_KEY_Pause, 0, INV, INV, INV, PRESS, "\033[57362u");
    kkey("Print", XKB_KEY_Print, 0, INV, INV, INV, PRESS, "\033[57361u");
    kkey("Menu", XKB_KEY_Menu, 0, INV, INV, INV, PRESS, "\033[57363u");
    kkey("Scroll Lock", XKB_KEY_Scroll_Lock, 0, INV, INV, INV, PRESS, "\033[57359u");
    kkey("KP_Enter", XKB_KEY_KP_Enter, NUM, '\r', INV, INV, PRESS, "\033[57414;129u");
    kkey("KP_1 with Num Lock is text", XKB_KEY_KP_1, NUM, '1', INV, INV, PRESS, "1");
    kkey("Ctrl+KP_1", XKB_KEY_KP_1, C | NUM, '1', INV, INV, PRESS, "\033[57400;133u");
    kkey("KP_End", XKB_KEY_KP_End, 0, INV, INV, INV, PRESS, "\033[57424u");
    kkey("KP_Begin", XKB_KEY_KP_Begin, 0, INV, INV, INV, PRESS, "\033[E");
    kkey("Shift alone", XKB_KEY_Shift_L, S, INV, INV, INV, PRESS, "");
    kkey("Caps Lock alone", XKB_KEY_Caps_Lock, 0, INV, INV, INV, PRESS, "");
    kkey("a with Caps Lock is text", 'A', CAPS, 'A', 'a', 'a', PRESS, "A");
    kkey("ctrl+a with Caps Lock", 'A', C | CAPS, 'A', 'a', 'a', PRESS, "\033[97;69u");
    kkey("Russian es is text", XKB_KEY_Cyrillic_es, 0, 0x441, 0x441, 'c', PRESS, "\xd1\x81");
    kkey("Russian ctrl+es", XKB_KEY_Cyrillic_es, C, 0x441, 0x441, 'c', PRESS, "\033[1089;5u");
    kkey("a release without flag 2", 'a', 0, 'a', 'a', 'a', RELEASE, "");
    kkey("a repeat without flag 2", 'a', 0, 'a', 'a', 'a', REPEAT, "a");
    kkey("Esc repeat without flag 2", XKB_KEY_Escape, 0, INV, INV, INV, REPEAT, "\033[27u");

    /* The old call sends a press, and lower-cases the key code. */
    key("old call: ctrl+c", 'c', 'c', C, 'c', "\033[99;5u");
    key("old call: ctrl+shift+C", 'C', 'C', C | S, 'C', "\033[99;6u");
    key("old call: Esc", XKB_KEY_Escape, XKB_KEY_NoSymbol, 0, INV, "\033[27u");

    /* Leaving the protocol gives the legacy encoding back. */
    put("\033[<u");
    kkey("ctrl+c after the pop", 'c', C, 'c', 'c', 'c', PRESS, "\x03");
    kkey("release after the pop", 'c', C, 'c', 'c', 'c', RELEASE, "");
    check(!tsm_vte_handle_key(vte, 'c', 'c', C, 'c', 'c', 'c', RELEASE),
          "a release without the protocol sends nothing");
}

/** Flag 2: repeat and release events. */
static void test_kitty_events(void)
{
    start();
    put("\033[=3u");
    kkey("press a", 'a', 0, 'a', 'a', 'a', PRESS, "a");
    kkey("repeat a", 'a', 0, 'a', 'a', 'a', REPEAT, "a");
    kkey("release a", 'a', 0, 'a', 'a', 'a', RELEASE, "\033[97;1:3u");
    kkey("release shift+a", 'A', S, 'A', 'a', 'a', RELEASE, "\033[97;2:3u");
    kkey("press Esc", XKB_KEY_Escape, 0, INV, INV, INV, PRESS, "\033[27u");
    kkey("repeat Esc", XKB_KEY_Escape, 0, INV, INV, INV, REPEAT, "\033[27;1:2u");
    kkey("release Esc", XKB_KEY_Escape, 0, INV, INV, INV, RELEASE, "\033[27;1:3u");
    kkey("press ctrl+c", 'c', C, 'c', 'c', 'c', PRESS, "\033[99;5u");
    kkey("repeat ctrl+c", 'c', C, 'c', 'c', 'c', REPEAT, "\033[99;5:2u");
    kkey("release ctrl+c", 'c', C, 'c', 'c', 'c', RELEASE, "\033[99;5:3u");
    kkey("press Up", XKB_KEY_Up, 0, INV, INV, INV, PRESS, "\033[A");
    kkey("repeat Up", XKB_KEY_Up, 0, INV, INV, INV, REPEAT, "\033[1;1:2A");
    kkey("release Up", XKB_KEY_Up, 0, INV, INV, INV, RELEASE, "\033[1;1:3A");
    kkey("release F5", XKB_KEY_F5, 0, INV, INV, INV, RELEASE, "\033[15;1:3~");
    kkey("release Enter sends nothing", XKB_KEY_Return, 0, INV, INV, INV, RELEASE, "");
    kkey("release Tab sends nothing", XKB_KEY_Tab, 0, INV, INV, INV, RELEASE, "");
    kkey("release Backspace sends nothing", XKB_KEY_BackSpace, 0, INV, INV, INV, RELEASE, "");
    kkey("repeat Enter", XKB_KEY_Return, 0, INV, INV, INV, REPEAT, "\r");
    kkey("release shift+Enter", XKB_KEY_Return, S, INV, INV, INV, RELEASE, "\033[13;2:3u");
    kkey("release Shift alone", XKB_KEY_Shift_L, 0, INV, INV, INV, RELEASE, "");

    /* Flag 2 without flag 1: presses are legacy, the rest are not. */
    put("\033[=2u");
    kkey("flag 2: press ctrl+c", 'c', C, 'c', 'c', 'c', PRESS, "\x03");
    kkey("flag 2: repeat ctrl+c", 'c', C, 'c', 'c', 'c', REPEAT, "\033[99;5:2u");
    kkey("flag 2: release ctrl+c", 'c', C, 'c', 'c', 'c', RELEASE, "\033[99;5:3u");
    kkey("flag 2: press Esc", XKB_KEY_Escape, 0, INV, INV, INV, PRESS, "\033");
    kkey("flag 2: repeat a", 'a', 0, 'a', 'a', 'a', REPEAT, "a");
}

/** Flag 4: the shifted key and the base layout's key. */
static void test_kitty_alternates(void)
{
    start();
    put("\033[=5u");
    kkey("ctrl+shift+a with alternates", 'A', C | S, 'A', 'a', 'a', PRESS, "\033[97:65;6u");
    kkey("ctrl+a: no shifted key without Shift", 'a', C, 'a', 'a', 'a', PRESS, "\033[97;5u");
    kkey("ctrl+a with Caps Lock: no shifted key", 'A', C | CAPS, 'A', 'a', 'a', PRESS,
         "\033[97;69u");
    kkey("Russian ctrl+C, the specification's example", XKB_KEY_Cyrillic_es, C, 0x441, 0x441, 'c',
         PRESS, "\033[1089::99;5u");
    kkey("Russian ctrl+shift+C", XKB_KEY_Cyrillic_ES, C | S, 0x421, 0x441, 'c', PRESS,
         "\033[1089:1057:99;6u");
    kkey("Russian alt+ef", XKB_KEY_Cyrillic_ef, A, 0x444, 0x444, 'a', PRESS, "\033[1092::97;3u");
    kkey("ctrl+shift+equal gives plus", '+', C | S, '+', '=', '=', PRESS, "\033[61:43;6u");
    kkey("Esc has no alternates", XKB_KEY_Escape, S, INV, INV, INV, PRESS, "\033[27;2u");
    kkey("Shift+F1 has no alternates", XKB_KEY_F1, S, INV, INV, INV, PRESS, "\033[1;2P");
    kkey("Russian es with alternates is text", XKB_KEY_Cyrillic_es, 0, 0x441, 0x441, 'c', PRESS,
         "\xd1\x81");

    put("\033[=7u");
    kkey("release of Russian ctrl+C", XKB_KEY_Cyrillic_es, C, 0x441, 0x441, 'c', RELEASE,
         "\033[1089::99;5:3u");
    kkey("release of shift+a", 'A', S, 'A', 'a', 'a', RELEASE, "\033[97:65;2:3u");
}

int main(void)
{
    test_requests();
    test_scosc();
    test_legacy_functional();
    test_legacy_c0();
    test_legacy_ctrl();
    test_legacy_keypad();
    test_kitty_spec();
    test_kitty_events();
    test_kitty_alternates();

    if (vte)
        tsm_vte_unref(vte);
    if (screen)
        tsm_screen_unref(screen);

    if (failures) {
        fprintf(stderr, "libtsm_keys_hosttest: %d failures\n", failures);
        return 1;
    }
    printf("libtsm_keys_hosttest: pass\n");
    return 0;
}
