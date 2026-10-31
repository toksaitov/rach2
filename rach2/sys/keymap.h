/*
 * keymap.h: the keyboard's translator, which turns key codes into text and
 * terminal sequences through keymaps in Linux's encoding. Plain C89 with no
 * kernel calls, so that the kernel's console, `term` and a host test all
 * compile keymap.c.
 */
#ifndef _KEYMAP_H_
#define _KEYMAP_H_

/*
 * A keymap is up to KM_MAX_TABLES tables of KM_NR_KEYS entries, as Linux's
 * console keeps it (keymaps(5)). Each table is one combination of the
 * modifiers, its index the sum of the weights 1 << KM_G_* of the modifiers in
 * force, held or locked: Shift and Control together are table 5. A table that
 * does not exist reads as KM_NOSUCHMAP.
 *
 * An entry is a 16-bit value in the encoding of KDGKBENT and KDSKBENT, which
 * kbd's `loadkeys` writes: KM_K(type, value), whose type is below 16, for a
 * typed keysym, and a code point `u` as KM_U(u), whose type is 16 or more.
 * The names here are Linux's with `KM_` before them, value for value, so that
 * a program may include both this header and <linux/keyboard.h>: KM_T_LATIN
 * is KT_LATIN, KM_G_SHIFT is KG_SHIFT, KM_HOLE is K_HOLE.
 */

#define KM_NR_KEYS    256 /* the entries of a table: key codes 0 to 255 */
#define KM_MAX_TABLES 256 /* the tables of a keymap */
#define KM_MAX_FUNC   256 /* the function strings */
#define KM_FUNC_BYTES 512 /* the longest function string, with its null */
#define KM_MAX_DIACR  256 /* the entries of the accent table */

#define KM_K(t, v)  (((t) << 8) | (v))
#define KM_TYPE(e)  ((e) >> 8)
#define KM_VALUE(e) ((e) & 0xFF)
#define KM_U(u)     ((u) ^ 0xF000) /* a code point's entry, and an entry's code point */
#define KM_IS_U(e)  (KM_TYPE(e) >= 16)

/* The entries' types, Linux's KT_*. */
#define KM_T_LATIN  0  /* a character of Latin-1 */
#define KM_T_FN     1  /* a function string, by its number */
#define KM_T_SPEC   2  /* an action of the console: Enter, Caps Lock, Compose */
#define KM_T_PAD    3  /* a key of the keypad */
#define KM_T_DEAD   4  /* a dead key, by the accent's number */
#define KM_T_CONS   5  /* a switch to a virtual console */
#define KM_T_CUR    6  /* a cursor key */
#define KM_T_SHIFT  7  /* a modifier, held while the key is */
#define KM_T_META   8  /* a character with Alt */
#define KM_T_ASCII  9  /* a digit of a character typed by its code */
#define KM_T_LOCK   10 /* a modifier that the key turns on and off */
#define KM_T_LETTER 11 /* a Latin-1 letter that Caps Lock shifts */
#define KM_T_SLOCK  12 /* a modifier held for the next key only */
#define KM_T_DEAD2  13 /* a dead key, by the accent's character */
#define KM_T_BRL    14 /* a Braille key */

/* The modifiers, Linux's KG_*: the bit of each in a table's index. */
#define KM_G_SHIFT     0
#define KM_G_ALTGR     1
#define KM_G_CTRL      2
#define KM_G_ALT       3
#define KM_G_SHIFTL    4
#define KM_G_SHIFTR    5
#define KM_G_CTRLL     6
#define KM_G_CTRLR     7
#define KM_G_CAPSSHIFT 8
#define KM_NR_SHIFT    9

/* KM_T_SPEC's entries this translator gives meaning to, Linux's K_*. */
#define KM_HOLE        KM_K(KM_T_SPEC, 0) /* nothing */
#define KM_ENTER       KM_K(KM_T_SPEC, 1) /* Enter */
#define KM_BREAK       KM_K(KM_T_SPEC, 5)
#define KM_CAPS        KM_K(KM_T_SPEC, 7)   /* Caps Lock */
#define KM_NUM         KM_K(KM_T_SPEC, 8)   /* Num Lock */
#define KM_HOLD        KM_K(KM_T_SPEC, 9)   /* Scroll Lock */
#define KM_SCROLLFORW  KM_K(KM_T_SPEC, 10)  /* the console's scrollback, down */
#define KM_SCROLLBACK  KM_K(KM_T_SPEC, 11)  /* and up */
#define KM_CAPSON      KM_K(KM_T_SPEC, 13)  /* Caps Lock on, never off */
#define KM_COMPOSE     KM_K(KM_T_SPEC, 14)  /* the next two keys compose */
#define KM_BARENUMLOCK KM_K(KM_T_SPEC, 19)  /* Num Lock always */
#define KM_NOSUCHMAP   KM_K(KM_T_SPEC, 127) /* an entry of a table that does not exist */

/* The bits of the locks KDGKBLED reports, `ledflags` below. */
#define KM_LED_SCROLL 0x01
#define KM_LED_NUM    0x02
#define KM_LED_CAPS   0x04

/* The modifiers of a result, as xterm numbers them: the parameter it sends
 * for a modified key is one more than their sum. */
#define KM_MOD_SHIFT 0x01
#define KM_MOD_ALT   0x02
#define KM_MOD_CTRL  0x04
#define KM_MOD_META  0x08

/* The terminal's modes `keymap_encode` encodes under, which the console
 * keeps (kio_key_modes) and `term` reads from libtsm. */
#define KM_MODE_CURSOR 0x01 /* DECCKM: the cursor keys send SS3 */
#define KM_MODE_KEYPAD 0x02 /* DECKPAM: the keypad sends its application sequences */

/* One entry of the accent table: `diacr` followed by `base` gives `result`,
 * all code points, as struct kbdiacruc. */
struct keymap_diacr {
    unsigned long diacr;
    unsigned long base;
    unsigned long result;
};

/*
 * Where the translator reads a keymap: the tables through `entry`, so that
 * the kernel reads its own and `term` asks the console with KDGKBENT as each
 * key is pressed; the strings and accents from arrays the caller keeps.
 */
struct keymap {
    /* The entry of key `keycode` in table `table`, KM_NOSUCHMAP when the
     * table does not exist. */
    unsigned int (*entry)(const void *ctx, unsigned int table, unsigned int keycode);
    const void *ctx;
    /* KM_MAX_FUNC null-terminated strings, a NULL one empty; NULL for none. */
    const char *const *func;
    const struct keymap_diacr *diacr; /* `ndiacr` entries */
    unsigned int ndiacr;
};

/* What one keyboard's translation remembers between keys. */
struct keymap_state {
    unsigned short down[KM_NR_SHIFT];      /* the keys holding each modifier down */
    unsigned char key_shift[KM_NR_KEYS];   /* modifier + 1 at press, 0 otherwise */
    unsigned char pressed[KM_NR_KEYS / 8]; /* physical keys, for duplicate presses */
    unsigned int shift;                    /* the held modifiers, a bit for each KM_G_* */
    unsigned int locks;                    /* the KM_T_LOCK modifiers on, likewise */
    unsigned int slocks;                   /* the KM_T_SLOCK modifiers on for the next key */
    unsigned int ledflags;                 /* Caps, Num and Scroll Lock, KM_LED_* */
    unsigned long diacr;                   /* the pending accent's code point, 0 for none */
    int compose;                           /* Compose was pressed: the next key is an accent */
    unsigned long npad;                    /* a code typed with Alt on the keypad so far */
    int npad_on;                           /* `npad` holds digits */
};

/* What a key gives, `kind` of a struct keymap_result. */
#define KM_RESULT_NONE    0 /* nothing: a modifier, a release, a pending accent */
#define KM_RESULT_TEXT    1 /* the code points `text`, `ntext` of them */
#define KM_RESULT_FUNC    2 /* the function string `func` */
#define KM_RESULT_SPECIAL 3 /* `key`, a KM_T_CUR, KM_T_PAD or KM_T_SPEC entry */
#define KM_RESULT_LOCKS   4 /* the locks changed: `ledflags` holds them */

struct keymap_result {
    int kind;              /* KM_RESULT_* */
    unsigned long text[2]; /* an accent that composed nothing comes before the key */
    unsigned int ntext;
    unsigned int func;     /* KM_RESULT_FUNC: the string's number */
    unsigned int key;      /* SPECIAL: keysym; TEXT: source Linux keycode */
    unsigned int mods;     /* the modifiers held, KM_MOD_*, for every kind */
    unsigned int ledflags; /* the locks after the key, KM_LED_* */
    int repeat;            /* the key was a repeat */
};

/**
 * Put `st` in its state at boot: nothing held, no lock on but those of
 * `ledflags`, KM_LED_*, no accent pending.
 */
void keymap_state_init(struct keymap_state *st, unsigned int ledflags);

/**
 * Translate one key event of one keyboard through `map`: `keycode` is Linux's
 * KEY_* code and `value` 0 for a release, 1 for a press and 2 for a repeat,
 * as evdev's. The modifiers, locks and accent of `st` change as the key says.
 * Fills `res` and returns its kind, KM_RESULT_NONE when the key gives nothing
 * to send.
 *
 * Any context; it calls only `map->entry`. The caller serializes access to
 * `st`, and keeps the strings and accents valid during the call. Absent
 * tables suppress keys but still recognize base-table modifiers. A release
 * uses the modifier recorded at press, even after a map or lock change.
 * Escape cancels Compose or an accent and sends ESC; Backspace cancels
 * either without sending. Other failed compositions send both code points.
 * Caps Lock inverts Shift for KT_LETTER and Unicode simple case pairs.
 */
int keymap_key(struct keymap_state *st, const struct keymap *map, unsigned int keycode, int value,
               struct keymap_result *res);

/**
 * Write the bytes a terminal sends for `res` into `buf`, of `size` bytes, as
 * xterm sends them, under `modes`, KM_MODE_*: text in UTF-8 with ESC before
 * it for Alt, a function string from `map`, the cursor and editing keys and
 * the keypad with `CSI 1;m` for their modifiers. Returns the bytes written,
 * 0 for a result that sends nothing or does not fit.
 *
 * No terminator is written, and a result that does not fit leaves `buf`
 * unchanged. UTF-8 accepts scalar values through U+10FFFF. Unknown keysyms
 * send nothing. A function string of at most KM_FUNC_BYTES - 1 bytes is
 * copied; a single CSI or SS3 key sequence receives xterm's modifier
 * parameter, and other strings are copied literally. Num Lock overrides
 * application keypad mode; Shift reverses Num Lock. Application keypad
 * modifiers use SS3 followed by the modifier parameter, as xterm does.
 * Numeric keypad characters ignore Ctrl and Alt. Shift+Page Up and Down
 * return the scrollback actions and send nothing.
 */
unsigned int keymap_encode(const struct keymap *map, const struct keymap_result *res,
                           unsigned int modes, char *buf, unsigned int size);

/*
 * The default keymap, built in: US, with Enter as CR and function strings of
 * xterm's. Its tables are `keymap_default_tables`, KM_MAX_TABLES of them, a
 * NULL one absent, which `keymap_default_entry` reads for a struct keymap.
 */
extern const unsigned short *const keymap_default_tables[KM_MAX_TABLES];
extern const char *const keymap_default_func[KM_MAX_FUNC];
extern const struct keymap_diacr keymap_default_diacr[];
extern const unsigned int keymap_default_ndiacr;

/**
 * The entry of `keycode` in table `table` of `ctx`, an array of KM_MAX_TABLES
 * table pointers such as `keymap_default_tables`: KM_NOSUCHMAP for a NULL
 * table, and KM_HOLE for a key code beyond the tables.
 */
unsigned int keymap_default_entry(const void *ctx, unsigned int table, unsigned int keycode);

#endif
