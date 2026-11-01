/*
 * keys.c: the console's keyboard in K_MEDIUMRAW, turned into libtsm's key
 * events through the console's keymap by the kernel's own translator, and
 * term's own keys: the scrollback, the font's size, selection and paste.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "term.h"

/*
 * The requests and structures of Linux's <linux/kd.h>, with its numbers,
 * which the console answers (rach2/sys/kd.h). They are spelt out here so
 * that term -H builds on a host without Linux's headers.
 */
#define TK_KDGKBMODE    0x4B44
#define TK_KDSKBMODE    0x4B45
#define TK_K_MEDIUMRAW  0x02
#define TK_KDGKBENT     0x4B46
#define TK_KDGKBSENT    0x4B48
#define TK_KDGKBDIACRUC 0x4BFA
#define TK_KDGKBLED     0x4B64
#define TK_KDSKBLED     0x4B65
#define TK_KDMKTONE     0x4B30  /* ms in the high 16 bits, divisor in the low */
#define TK_PIT_CLOCK    1193182 /* the clock the divisor divides, in Hz */

struct tk_kbentry {
    unsigned char kb_table;
    unsigned char kb_index;
    unsigned short kb_value;
};

struct tk_kbsentry {
    unsigned char kb_func;
    unsigned char kb_string[512];
};

struct tk_kbdiacruc {
    unsigned int diacr, base, result;
};

struct tk_kbdiacrsuc {
    unsigned int kb_cnt;
    struct tk_kbdiacruc kbdiacruc[256];
};

/* Linux's KEY_* codes of the keys term reads by their code. */
#define CODE_KPENTER 96
#define CODE_SYSRQ   99
#define CODE_COMPOSE 127

/* XKB's keysyms that libtsm and the selection mode read. */
#define SYM_NONE         0x0000u
#define SYM_BACKSPACE    0xff08u
#define SYM_TAB          0xff09u
#define SYM_RETURN       0xff0du
#define SYM_ESCAPE       0xff1bu
#define SYM_HOME         0xff50u
#define SYM_LEFT         0xff51u
#define SYM_UP           0xff52u
#define SYM_RIGHT        0xff53u
#define SYM_DOWN         0xff54u
#define SYM_PAGE_UP      0xff55u
#define SYM_PAGE_DOWN    0xff56u
#define SYM_END          0xff57u
#define SYM_INSERT       0xff63u
#define SYM_MENU         0xff67u
#define SYM_ISO_LEFT_TAB 0xfe20u
#define SYM_KP_ENTER     0xff8du
#define SYM_KP_HOME      0xff95u
#define SYM_KP_LEFT      0xff96u
#define SYM_KP_UP        0xff97u
#define SYM_KP_RIGHT     0xff98u
#define SYM_KP_DOWN      0xff99u
#define SYM_KP_PAGE_UP   0xff9au
#define SYM_KP_PAGE_DOWN 0xff9bu
#define SYM_KP_END       0xff9cu
#define SYM_KP_BEGIN     0xff9du
#define SYM_KP_INSERT    0xff9eu
#define SYM_KP_DELETE    0xff9fu
#define SYM_KP_MULTIPLY  0xffaau
#define SYM_KP_ADD       0xffabu
#define SYM_KP_SEPARATOR 0xffacu
#define SYM_KP_SUBTRACT  0xffadu
#define SYM_KP_DECIMAL   0xffaeu
#define SYM_KP_DIVIDE    0xffafu
#define SYM_KP_0         0xffb0u
#define SYM_F1           0xffbeu
#define SYM_DELETE       0xffffu

/* The modifier bits of a table's index that are held, not locked, and that
 * the character before Ctrl and Alt, and with no modifier, leave out. */
#define HELD_CTRL_ALT \
    ((1u << KM_G_CTRL) | (1u << KM_G_CTRLL) | (1u << KM_G_CTRLR) | (1u << KM_G_ALT))
#define HELD_ALL                                                                     \
    (HELD_CTRL_ALT | (1u << KM_G_SHIFT) | (1u << KM_G_ALTGR) | (1u << KM_G_SHIFTL) | \
     (1u << KM_G_SHIFTR))

/* ---- Where the keymap is read ---- */

/** A table entry of the console by KDGKBENT; KM_HOLE when it cannot be read. */
static unsigned console_get(const struct term_keys *k, unsigned table, unsigned code)
{
    struct tk_kbentry e;

    e.kb_table = (unsigned char)table;
    e.kb_index = (unsigned char)code;
    e.kb_value = 0;
    if (ioctl(k->fd, TK_KDGKBENT, &e) < 0)
        return KM_HOLE;
    return e.kb_value;
}

/**
 * The entry of key `code` in table `table` of `k`'s keymap: the console's,
 * or term -H's own. KM_NOSUCHMAP for a table that does not exist, as the
 * translator expects; KDGKBENT gives it only at key 0 and K_HOLE elsewhere,
 * so a hole is told from a missing table by key 0.
 */
static unsigned keymap_get(const struct term_keys *k, unsigned table, unsigned code)
{
    unsigned v;

    if (table >= KM_MAX_TABLES || code >= KM_NR_KEYS)
        return KM_HOLE;
    if (k->fd < 0) {
        if (k->freed[table])
            return KM_NOSUCHMAP;
        if (k->tables[table])
            return k->tables[table][code];
        return keymap_default_entry(keymap_default_tables, table, code);
    }
    v = console_get(k, table, code);
    if (v == KM_HOLE && code != 0 && console_get(k, table, 0) == KM_NOSUCHMAP)
        return KM_NOSUCHMAP;
    return v;
}

/*
 * A view of the keymap for one translation, through which the translator
 * reads the tables. A view other than VIEW_REAL answers for another table
 * than the translator asks for, so that the same translator, run on a copy
 * of the state, gives the characters the Kitty protocol and Ctrl need.
 */
enum view {
    VIEW_REAL,    /* the tables as they are */
    VIEW_PLAIN,   /* without Ctrl and Alt: the character before them */
    VIEW_BARE,    /* without any held modifier, in the layout in use */
    VIEW_BASE,    /* without any modifier or lock: the base layout */
    VIEW_SHIFTED, /* the base layout with Shift alone, for Ctrl's ASCII key */
};

struct view_ctx {
    const struct term_keys *k;
    unsigned locks; /* the locked modifiers of the state translated */
    enum view view;
};

/** The struct keymap callback of a view. */
static unsigned int view_entry(const void *ctx, unsigned int table, unsigned int keycode)
{
    const struct view_ctx *v = ctx;
    unsigned held = table ^ v->locks;

    switch (v->view) {
    case VIEW_REAL:
        break;
    case VIEW_PLAIN:
        table = (held & ~HELD_CTRL_ALT) ^ v->locks;
        break;
    case VIEW_BARE:
        table = (held & ~HELD_ALL) ^ v->locks;
        break;
    case VIEW_BASE:
        table = 0;
        break;
    case VIEW_SHIFTED:
        table = held & (1u << KM_G_SHIFT);
        break;
    }
    return keymap_get(v->k, table, keycode);
}

/** Make `map` read the keymap of `k` through view `view` of state `st`. */
static void view_map(struct keymap *map, struct view_ctx *ctx, const struct term_keys *k,
                     const struct keymap_state *st, enum view view)
{
    ctx->k = k;
    ctx->locks = st->locks;
    ctx->view = view;
    map->entry = view_entry;
    map->ctx = ctx;
    map->func = NULL;
    if (k->fd < 0 && k->ndiacr == 0) {
        map->diacr = keymap_default_diacr;
        map->ndiacr = keymap_default_ndiacr;
    } else {
        map->diacr = k->diacr;
        map->ndiacr = k->ndiacr;
    }
}

/**
 * The character key `code` gives as a press through view `view` of state
 * `st`, with Caps Lock off when `nocaps` is set; TSM_VTE_INVALID when it
 * gives none. The state is copied, so `st` does not change.
 */
static uint32_t view_char(const struct term_keys *k, const struct keymap_state *st, unsigned code,
                          enum view view, bool nocaps)
{
    struct keymap_state copy = *st;
    struct keymap_result res;
    struct view_ctx ctx;
    struct keymap map;

    if (nocaps)
        copy.ledflags &= ~KM_LED_CAPS;
    copy.diacr = 0;
    copy.compose = 0;
    copy.pressed[code >> 3] &= (unsigned char)~(1u << (code & 7));
    view_map(&map, &ctx, k, &copy, view);
    if (keymap_key(&copy, &map, code, 1, &res) != KM_RESULT_TEXT || res.ntext == 0)
        return TSM_VTE_INVALID;
    return (uint32_t)res.text[res.ntext - 1];
}

/**
 * Read the console's accent table into `k`, so that a dead key composes with
 * the table `loadkeys` last loaded. Left as it was when the request fails.
 */
static void read_accents(struct term_keys *k)
{
    struct tk_kbdiacrsuc *a;
    unsigned i;

    if (k->fd < 0)
        return;
    a = malloc(sizeof(*a));
    if (!a)
        return;
    if (ioctl(k->fd, TK_KDGKBDIACRUC, a) == 0) {
        if (a->kb_cnt > KM_MAX_DIACR)
            a->kb_cnt = KM_MAX_DIACR;
        for (i = 0; i < a->kb_cnt; i++) {
            k->diacr[i].diacr = a->kbdiacruc[i].diacr;
            k->diacr[i].base = a->kbdiacruc[i].base;
            k->diacr[i].result = a->kbdiacruc[i].result;
        }
        k->ndiacr = a->kb_cnt;
    }
    free(a);
}

/**
 * Function string `n` of the keymap into `buf`, of KM_FUNC_BYTES bytes:
 * the console's by KDGKBSENT, or term -H's. Returns its length.
 */
static size_t read_string(const struct term_keys *k, unsigned n, char *buf)
{
    struct tk_kbsentry e;
    const char *src = NULL;
    size_t len;

    buf[0] = '\0';
    if (n >= KM_MAX_FUNC)
        return 0;
    if (k->fd < 0) {
        src = k->func[n] ? k->func[n] : keymap_default_func[n];
    } else {
        memset(&e, 0, sizeof(e));
        e.kb_func = (unsigned char)n;
        if (ioctl(k->fd, TK_KDGKBSENT, &e) < 0)
            return 0;
        e.kb_string[sizeof(e.kb_string) - 1] = '\0';
        src = (const char *)e.kb_string;
    }
    if (!src)
        return 0;
    len = strlen(src);
    if (len > KM_FUNC_BYTES - 1)
        len = KM_FUNC_BYTES - 1;
    memcpy(buf, src, len);
    buf[len] = '\0';
    return len;
}

/* ---- The locks ---- */

/**
 * Take the console's locks, KDGKBLED's, when they differ from the
 * translator's, as after `setleds`. term -H keeps its own.
 */
static void locks_in(struct term_keys *k)
{
    unsigned char b = 0;

    if (k->fd < 0 || ioctl(k->fd, TK_KDGKBLED, &b) < 0)
        return;
    k->kbled = b;
    k->state.ledflags = b & 7u;
}

/** Give the console the translator's locks, so its LEDs follow them. */
static void locks_out(struct term_keys *k)
{
    unsigned long b;

    if (k->fd < 0)
        return;
    b = (k->kbled & 0x70u) | (k->state.ledflags & 7u);
    if (ioctl(k->fd, TK_KDSKBLED, b) == 0)
        k->kbled = (unsigned)b;
}

/* ---- Translation ---- */

/**
 * libtsm's modifier bits for the translator's KM_MOD_* bits `km` and its
 * locks `leds`, KM_LED_*. The Kitty protocol reports Caps Lock and Num Lock
 * on every key, and libtsm's legacy encoding reads only Num Lock, on the
 * keypad.
 */
static unsigned tsm_mods(unsigned km, unsigned leds)
{
    unsigned m = 0;

    if (leds & KM_LED_CAPS)
        m |= TSM_LOCK_MASK;
    if (leds & KM_LED_NUM)
        m |= TSM_NUM_LOCK_MASK;

    if (km & KM_MOD_SHIFT)
        m |= TSM_SHIFT_MASK;
    if (km & KM_MOD_ALT)
        m |= TSM_ALT_MASK;
    if (km & KM_MOD_CTRL)
        m |= TSM_CONTROL_MASK;
    if (km & KM_MOD_META)
        m |= TSM_LOGO_MASK;
    return m;
}

/** XKB's keysym of character `c`, as xkbcommon gives one for a code point. */
static uint32_t char_keysym(uint32_t c)
{
    if ((c >= 0x20 && c < 0x7F) || (c >= 0xA0 && c <= 0xFF))
        return c;
    return 0x01000000u | c;
}

/**
 * Fill `key` for a key that gives text `c` under modifiers `mods`, libtsm's:
 * the control characters of Escape, Tab, Enter and Backspace become their
 * keys, and the characters stay with the text keys.
 */
static void text_key(struct term_key *key, uint32_t c, unsigned mods)
{
    key->mods = mods;
    key->unicode = c;
    key->ascii = SYM_NONE;
    switch (c) {
    case 0x1B:
        key->keysym = SYM_ESCAPE;
        break;
    case 0x09:
        key->keysym = (mods & TSM_SHIFT_MASK) ? SYM_ISO_LEFT_TAB : SYM_TAB;
        break;
    case 0x0D:
        key->keysym = SYM_RETURN;
        break;
    case 0x08:
    case 0x7F:
        key->keysym = SYM_BACKSPACE;
        break;
    default:
        key->keysym = char_keysym(c);
        return;
    }
    key->unicode = TSM_VTE_INVALID;
}

/* The keysym of each function string the built-in map gives, by number. */
static const uint32_t func_keysyms[] = {
    SYM_F1,      SYM_F1 + 1,  SYM_F1 + 2,  SYM_F1 + 3,  SYM_F1 + 4,   SYM_F1 + 5,  SYM_F1 + 6,
    SYM_F1 + 7,  SYM_F1 + 8,  SYM_F1 + 9,  SYM_F1 + 10, SYM_F1 + 11,  SYM_F1 + 12, SYM_F1 + 13,
    SYM_F1 + 14, SYM_F1 + 15, SYM_F1 + 16, SYM_F1 + 17, SYM_F1 + 18,  SYM_F1 + 19, SYM_HOME,
    SYM_INSERT,  SYM_DELETE,  SYM_END,     SYM_PAGE_UP, SYM_PAGE_DOWN};

/**
 * The key of xterm's `ESC [ n ~` for number `n`, F5 to F12 and the editing
 * keys, into `keysym`. Returns false for a number with no key.
 */
static bool tilde_key(unsigned long n, uint32_t *keysym)
{
    static const unsigned tilde[] = {15, 17, 18, 19, 20, 21, 23, 24, 2, 3, 5, 6};
    static const uint32_t tilde_syms[] = {SYM_F1 + 4, SYM_F1 + 5, SYM_F1 + 6,  SYM_F1 + 7,
                                          SYM_F1 + 8, SYM_F1 + 9, SYM_F1 + 10, SYM_F1 + 11,
                                          SYM_INSERT, SYM_DELETE, SYM_PAGE_UP, SYM_PAGE_DOWN};
    unsigned i;

    for (i = 0; i < sizeof(tilde) / sizeof(tilde[0]); i++) {
        if (tilde[i] == n) {
            *keysym = tilde_syms[i];
            return true;
        }
    }
    return false;
}

/** libtsm's modifiers of xterm's modifier parameter `m`, 1 and the bits. */
static unsigned xterm_mods(unsigned long m)
{
    m -= 1;
    return ((m & 1) ? TSM_SHIFT_MASK : 0) | ((m & 2) ? TSM_ALT_MASK : 0) |
           ((m & 4) ? TSM_CONTROL_MASK : 0) | ((m & 8) ? TSM_LOGO_MASK : 0);
}

/**
 * The key and modifiers, libtsm's, of `str` when it is xterm's sequence of
 * a function or editing key, as the maps' `xterm-strings` give Shift+F1 as
 * F13's `ESC [ 1 ; 2 P`: `ESC O` or `ESC [ 1 ;` m with P to S, or `ESC [`
 * n, `;` m and `~`, or `ESC [ H` and `ESC [ F`, where m is 1 and the
 * modifier bits. Returns false for any other string.
 */
static bool xterm_key(const char *str, uint32_t *keysym, unsigned *mods)
{
    unsigned long n = 1;
    unsigned long m = 1;
    char *end;

    if (str[0] != '\033')
        return false;
    if (str[1] == 'O' && str[2] >= 'P' && str[2] <= 'S' && str[3] == '\0') {
        *keysym = SYM_F1 + (uint32_t)(str[2] - 'P');
        *mods = 0;
        return true;
    }
    if (str[1] != '[')
        return false;
    str += 2;
    if (*str >= '0' && *str <= '9') {
        n = strtoul(str, &end, 10);
        str = end;
        if (*str == ';') {
            m = strtoul(str + 1, &end, 10);
            if (end == str + 1 || m < 1 || m > 16)
                return false;
            str = end;
        }
    }
    if (str[0] == '\0' || str[1] != '\0')
        return false;
    if (*str == '~') {
        if (!tilde_key(n, keysym))
            return false;
    } else if (n != 1) {
        return false;
    } else if (*str >= 'P' && *str <= 'S' && m > 1) {
        *keysym = SYM_F1 + (uint32_t)(*str - 'P');
    } else if (*str == 'H' || *str == 'F') {
        *keysym = *str == 'H' ? SYM_HOME : SYM_END;
    } else {
        return false;
    }
    *mods = xterm_mods(m);
    return true;
}

/**
 * A keypad key, KM_T_PAD value `v`, into `key`. With Num Lock on, which
 * Shift reverses, the digits and the point give their characters; with it
 * off they are the cursor and editing keys they are marked with. libtsm
 * sees Num Lock itself in `mods`, which keeps the keypad numeric in
 * application mode. Returns false for a value with no key.
 */
static bool pad_key(struct term_key *key, unsigned v, unsigned mods)
{
    static const uint32_t nav[10] = {SYM_KP_INSERT, SYM_KP_END,    SYM_KP_DOWN,  SYM_KP_PAGE_DOWN,
                                     SYM_KP_LEFT,   SYM_KP_BEGIN,  SYM_KP_RIGHT, SYM_KP_HOME,
                                     SYM_KP_UP,     SYM_KP_PAGE_UP};
    bool num = (mods & TSM_NUM_LOCK_MASK) != 0;
    bool digits = num != ((mods & TSM_SHIFT_MASK) != 0);

    key->mods = mods;
    key->ascii = SYM_NONE;
    key->unicode = TSM_VTE_INVALID;
    if (v <= 9) {
        key->keysym = digits ? SYM_KP_0 + v : nav[v];
        if (digits)
            key->unicode = '0' + v;
        return true;
    }
    switch (v) {
    case 10:
        key->keysym = SYM_KP_ADD;
        key->unicode = '+';
        return true;
    case 11:
        key->keysym = SYM_KP_SUBTRACT;
        key->unicode = '-';
        return true;
    case 12:
        key->keysym = SYM_KP_MULTIPLY;
        key->unicode = '*';
        return true;
    case 13:
        key->keysym = SYM_KP_DIVIDE;
        key->unicode = '/';
        return true;
    case 14:
        key->keysym = SYM_KP_ENTER;
        return true;
    case 15:
        key->keysym = SYM_KP_SEPARATOR;
        key->unicode = ',';
        return true;
    case 16:
        key->keysym = digits ? SYM_KP_DECIMAL : SYM_KP_DELETE;
        if (digits)
            key->unicode = '.';
        return true;
    case 17:
        key->keysym = 0x00B1;
        key->unicode = 0x00B1;
        return true;
    case 18:
    case 19:
        key->keysym = v == 18 ? '(' : ')';
        key->unicode = key->keysym;
        return true;
    }
    return false;
}

/**
 * Turn the translator's text result `res`, or its result of nothing, for key
 * `code` into `key`, as for make_key. An accent that composed nothing goes
 * straight to `child` when it is not NULL.
 */
static bool text_result(struct term_keys *k, struct term_screen *child,
                        const struct keymap_state *before, unsigned code,
                        const struct keymap_result *res, struct term_key *key)
{
    struct term_key accent;
    uint32_t c;

    if (res->mods & (KM_MOD_CTRL | KM_MOD_ALT))
        c = view_char(k, before, code, VIEW_PLAIN, false);
    else if (res->kind == KM_RESULT_TEXT)
        c = (uint32_t)res->text[res->ntext - 1];
    else
        return false;
    if (c == TSM_VTE_INVALID)
        return false;
    if (child && res->kind == KM_RESULT_TEXT && res->ntext == 2) {
        text_key(&accent, (uint32_t)res->text[0], 0);
        accent.unshifted = accent.base = TSM_VTE_INVALID;
        term_screen_key(child, &accent, TSM_VTE_KEY_PRESS);
    }
    text_key(key, c, tsm_mods(res->mods, res->ledflags));
    if (key->unicode != TSM_VTE_INVALID) {
        key->unshifted = view_char(k, before, code, VIEW_BARE, true);
        key->base = view_char(k, before, code, VIEW_BASE, true);
    }
    c = view_char(k, before, code, VIEW_SHIFTED, true);
    key->ascii = c < 0x80 ? c : SYM_NONE;
    return true;
}

/**
 * Turn function string `func` of key `code` into `key`, whose modifiers are
 * set: the built-in map's strings and xterm's sequences of keys become their
 * keys, and any other string goes straight to `child` when it is not NULL.
 * Returns false when the key sends nothing more.
 */
static bool func_result(struct term_keys *k, struct term_screen *child, unsigned code,
                        unsigned func, struct term_key *key)
{
    unsigned mods = key->mods;
    char str[KM_FUNC_BYTES];
    size_t n;

    n = read_string(k, func, str);
    if (code == CODE_COMPOSE && func == 15 && strcmp(str, keymap_default_func[15]) == 0) {
        key->keysym = SYM_MENU;
        return true;
    }
    if (func < sizeof(func_keysyms) / sizeof(func_keysyms[0]) &&
        strcmp(str, keymap_default_func[func]) == 0) {
        key->keysym = func_keysyms[func];
        return true;
    }
    if (xterm_key(str, &key->keysym, &key->mods)) {
        key->mods |= mods;
        return true;
    }
    if (child && n > 0)
        term_screen_send(child, str, n);
    return false;
}

/**
 * Turn the translator's result `res` for key `code` into `key`, with the
 * characters before Ctrl and Alt and without modifiers read through views
 * of `before`, the state before the key. When `child` is not NULL, as when
 * the selection mode is off, writes a function string that is neither the
 * built-in one nor xterm's sequence of a key, and an accent that composed
 * nothing, straight to the child through that screen. Returns false when
 * the key sends nothing more.
 */
static bool make_key(struct term_keys *k, struct term_screen *child,
                     const struct keymap_state *before, unsigned code,
                     const struct keymap_result *res, struct term_key *key)
{
    unsigned mods = tsm_mods(res->mods, res->ledflags);
    unsigned type = KM_TYPE(res->key);
    unsigned v = KM_VALUE(res->key);

    key->unshifted = TSM_VTE_INVALID;
    key->base = TSM_VTE_INVALID;
    if (res->kind == KM_RESULT_TEXT || res->kind == KM_RESULT_NONE)
        return text_result(k, child, before, code, res, key);
    key->mods = mods;
    key->ascii = SYM_NONE;
    key->unicode = TSM_VTE_INVALID;
    if (res->kind == KM_RESULT_FUNC)
        return func_result(k, child, code, res->func, key);
    if (res->kind != KM_RESULT_SPECIAL)
        return false;
    if (res->key == KM_ENTER) {
        key->keysym = code == CODE_KPENTER ? SYM_KP_ENTER : SYM_RETURN;
        return true;
    }
    if (type == KM_T_CUR && v <= 3) {
        static const uint32_t cur[4] = {SYM_DOWN, SYM_LEFT, SYM_RIGHT, SYM_UP};
        key->keysym = cur[v];
        return true;
    }
    if (type == KM_T_PAD)
        return pad_key(key, v, mods);
    return false;
}

/**
 * Act on the scrollback's keys, Shift with Page Up and Page Down: a page of
 * the scrollback, or with Ctrl the font at 2x and at 1x. On the alternate
 * screen, which has no scrollback, the key goes to the child.
 * Returns false when it is the child's.
 */
static bool scroll_key(struct term_keys *k, struct term_screen *s, const struct keymap_result *res,
                       struct term_key *key)
{
    bool up = res->key == KM_SCROLLBACK;
    unsigned cols;
    unsigned rows;

    if (res->mods & KM_MOD_CTRL) {
        k->scale = up ? 2 : 1;
        return true;
    }
    if (term_screen_alternate(s)) {
        key->keysym = up ? SYM_PAGE_UP : SYM_PAGE_DOWN;
        key->ascii = SYM_NONE;
        key->mods = tsm_mods(res->mods, res->ledflags);
        key->unicode = key->unshifted = key->base = TSM_VTE_INVALID;
        return false;
    }
    term_screen_size(s, &cols, &rows);
    term_screen_scroll(s, up ? (int)rows : -(int)rows);
    return true;
}

void term_keys_event(struct term_keys *k, struct term_screen *s, struct term_select *sel,
                     unsigned code, bool press)
{
    unsigned char bit = (unsigned char)(1u << (code & 7));
    struct keymap_state before;
    struct keymap_result res;
    struct term_key_sent *sent;
    struct view_ctx ctx;
    struct keymap map;
    struct term_key key;
    unsigned old_leds;
    bool repeat;
    int value;

    if (code > TERM_KEYS_MAX)
        return;
    sent = &k->sent[code];
    repeat = press && (k->down[code >> 3] & bit);
    if (press)
        k->down[code >> 3] |= bit;
    else
        k->down[code >> 3] &= (unsigned char)~bit;
    value = !press ? 0 : repeat ? 2 : 1;

    locks_in(k);
    if (k->state.diacr || k->state.compose)
        read_accents(k);

    /* Print Screen is term's: the selection mode, and with Shift paste. */
    if (code == CODE_SYSRQ) {
        if (value != 1)
            return;
        if (k->state.shift & ((1u << KM_G_SHIFT) | (1u << KM_G_SHIFTL) | (1u << KM_G_SHIFTR)))
            term_select_paste(sel, s);
        else if (sel->active)
            term_select_clear(sel, s);
        else
            term_select_start(sel, s);
        return;
    }

    before = k->state;
    old_leds = k->state.ledflags;
    view_map(&map, &ctx, k, &k->state, VIEW_REAL);
    keymap_key(&k->state, &map, code, value, &res);
    if (k->state.ledflags != old_leds)
        locks_out(k);

    if (value == 0) {
        if (sent->valid && !sel->active)
            term_screen_key(s, &sent->key, TSM_VTE_KEY_RELEASE);
        sent->valid = false;
        return;
    }
    if (res.kind == KM_RESULT_SPECIAL && (res.key == KM_SCROLLBACK || res.key == KM_SCROLLFORW)) {
        if (scroll_key(k, s, &res, &key)) {
            sent->valid = false;
            return;
        }
    } else if (!make_key(k, sel->active ? NULL : s, &before, code, &res, &key)) {
        if (value == 1)
            sent->valid = false;
        return;
    }
    if (sel->active) {
        term_select_key(sel, s, key.keysym, key.mods);
        sent->valid = false;
        return;
    }
    term_screen_key(s, &key, repeat ? TSM_VTE_KEY_REPEAT : TSM_VTE_KEY_PRESS);
    sent->valid = true;
    sent->key = key;
}

/* ---- The console ---- */

int term_keys_open(struct term_keys *k, const char *path)
{
    struct termios raw;
    int mode = -1;
    int r;

    term_keys_init_host(k);
    k->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (k->fd < 0)
        return -errno;
    if (ioctl(k->fd, TK_KDGKBMODE, &mode) < 0 || tcgetattr(k->fd, &k->termios) < 0) {
        r = -errno;
        close(k->fd);
        k->fd = -1;
        return r;
    }
    k->saved_termios = true;
    raw = k->termios;
    raw.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(k->fd, TCSANOW, &raw) < 0 || ioctl(k->fd, TK_KDSKBMODE, TK_K_MEDIUMRAW) < 0) {
        r = -errno;
        term_keys_close(k);
        return r;
    }
    k->saved_mode = mode;
    locks_in(k);
    read_accents(k);
    return 0;
}

void term_keys_init_host(struct term_keys *k)
{
    memset(k, 0, sizeof(*k));
    k->fd = -1;
    k->saved_mode = -1;
    keymap_state_init(&k->state, 0);
}

int term_keys_read(struct term_keys *k, struct term_screen *s, struct term_select *sel)
{
    unsigned char buf[64];
    unsigned code;
    ssize_t n;
    ssize_t i;
    unsigned char b;

    for (;;) {
        n = read(k->fd, buf, sizeof(buf));
        if (n < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
        if (n == 0)
            return 0;
        for (i = 0; i < n; i++) {
            b = buf[i];
            /* A code of 128 or more is 0x00 or 0x80, then its high and its
             * low seven bits, each with 0x80 set. */
            if (k->nraw > 0 || (b & 0x7F) == 0) {
                k->raw[k->nraw++] = b;
                if (k->nraw < 3)
                    continue;
                k->nraw = 0;
                code = (unsigned)(k->raw[1] & 0x7F) << 7 | (k->raw[2] & 0x7F);
                term_keys_event(k, s, sel, code, !(k->raw[0] & 0x80));
                continue;
            }
            term_keys_event(k, s, sel, b & 0x7Fu, !(b & 0x80));
        }
    }
}

int term_keys_set_entry(struct term_keys *k, unsigned table, unsigned code, unsigned value)
{
    unsigned short *t;
    unsigned i;

    if (table >= KM_MAX_TABLES || code >= KM_NR_KEYS || value > 0xFFFFu)
        return -EINVAL;
    if (code == 0) {
        if (value == KM_NOSUCHMAP && table != 0) {
            free(k->tables[table]);
            k->tables[table] = NULL;
            k->freed[table] = true;
        }
        return 0;
    }
    if (!k->tables[table]) {
        t = malloc(KM_NR_KEYS * sizeof(t[0]));
        if (!t)
            return -ENOMEM;
        for (i = 0; i < KM_NR_KEYS; i++)
            t[i] = (unsigned short)keymap_get(k, table, i);
        if (k->freed[table])
            for (i = 0; i < KM_NR_KEYS; i++)
                t[i] = (unsigned short)KM_HOLE;
        k->tables[table] = t;
        k->freed[table] = false;
    }
    k->tables[table][code] = (unsigned short)value;
    return 0;
}

int term_keys_set_string(struct term_keys *k, unsigned n, const char *text)
{
    char *copy;

    if (n >= KM_MAX_FUNC || strlen(text) >= KM_FUNC_BYTES)
        return -EINVAL;
    copy = malloc(strlen(text) + 1);
    if (!copy)
        return -ENOMEM;
    strcpy(copy, text);
    free(k->func[n]);
    k->func[n] = copy;
    return 0;
}

void term_keys_reset(struct term_keys *k)
{
    unsigned locks = k->state.locks;
    unsigned leds = k->state.ledflags;

    memset(k->down, 0, sizeof(k->down));
    memset(k->sent, 0, sizeof(k->sent));
    k->nraw = 0;
    keymap_state_init(&k->state, leds);
    k->state.locks = locks;
    locks_in(k);
}

int term_keys_tone(struct term_keys *k, unsigned hz, unsigned ms)
{
    unsigned long divisor;

    if (k->fd < 0)
        return -ENOTTY;
    if (hz == 0 || ms == 0)
        return 0;
    divisor = TK_PIT_CLOCK / hz;
    if (divisor == 0 || divisor > 0xFFFF)
        return 0;
    if (ms > 0xFFFF)
        ms = 0xFFFF;
    if (ioctl(k->fd, TK_KDMKTONE, (unsigned long)ms << 16 | divisor) < 0)
        return -errno;
    return 0;
}

void term_keys_close(struct term_keys *k)
{
    unsigned i;

    if (k->fd >= 0) {
        if (k->saved_mode >= 0)
            ioctl(k->fd, TK_KDSKBMODE, k->saved_mode);
        if (k->saved_termios)
            tcsetattr(k->fd, TCSANOW, &k->termios);
        close(k->fd);
    }
    for (i = 0; i < KM_MAX_TABLES; i++)
        free(k->tables[i]);
    for (i = 0; i < KM_MAX_FUNC; i++)
        free(k->func[i]);
    memset(k->tables, 0, sizeof(k->tables));
    memset(k->func, 0, sizeof(k->func));
    k->fd = -1;
    k->saved_mode = -1;
    k->saved_termios = false;
}
