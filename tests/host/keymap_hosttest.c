/*
 * keymap_hosttest.c: Linux keymap state and xterm bytes on text-loaded maps.
 * host-deps: rach2/sys/keymap.c
 */
#include "keymap.h"

extern int printf(const char *format, ...);
extern int sscanf(const char *s, const char *format, ...);

static int fails;
static unsigned short tables[KM_MAX_TABLES][KM_NR_KEYS];
static unsigned char present[KM_MAX_TABLES];
static struct keymap map;
static struct keymap_state state;
static struct keymap_result result;

#define CHECK(c)                                                      \
    do {                                                              \
        if (!(c)) {                                                   \
            printf("keymap-test: fail: line %d: %s\n", __LINE__, #c); \
            fails++;                                                  \
        }                                                             \
    } while (0)

/** Read a table populated by the text fixtures, or report a missing table. */
static unsigned int entry(const void *ctx, unsigned int table, unsigned int key)
{
    (void)ctx;
    if (key >= KM_NR_KEYS) {
        return KM_HOLE;
    }
    if (table >= KM_MAX_TABLES || !present[table]) {
        return KM_NOSUCHMAP;
    }
    return tables[table][key];
}

/** Load hexadecimal table, decimal keycode and hexadecimal keysym triples. */
static void load(const char *s)
{
    unsigned int table, key, symbol;
    int n;

    while (*s) {
        n = 0;
        if (sscanf(s, "%x %u %x%n", &table, &key, &symbol, &n) != 3 || !n ||
            table >= KM_MAX_TABLES || key >= KM_NR_KEYS || symbol > 65535) {
            CHECK(0);
            return;
        }
        tables[table][key] = (unsigned short)symbol;
        present[table] = 1;
        s += n;
        while (*s == '\n' || *s == ' ') {
            s++;
        }
    }
}

/** Start a fresh mutable US map and translator without touching any files. */
static void reset(void)
{
    unsigned int t, k;

    for (t = 0; t < KM_MAX_TABLES; t++) {
        present[t] = keymap_default_tables[t] != 0;
        for (k = 0; k < KM_NR_KEYS; k++) {
            tables[t][k] = present[t] ? keymap_default_tables[t][k] : KM_HOLE;
        }
    }
    map.entry = entry;
    map.ctx = 0;
    map.func = keymap_default_func;
    map.diacr = keymap_default_diacr;
    map.ndiacr = keymap_default_ndiacr;
    keymap_state_init(&state, 0);
}

/** Send one event and retain the result for the byte and state assertions. */
static int event(unsigned int key, int value)
{
    return keymap_key(&state, &map, key, value, &result);
}

/** Type a complete key stroke, retaining the press result. */
static void tap(unsigned int key)
{
    struct keymap_result release;

    event(key, 1);
    keymap_key(&state, &map, key, 0, &release);
}

/** Require a text result of one code point. */
static void point(unsigned long c)
{
    CHECK(result.kind == KM_RESULT_TEXT && result.ntext == 1 && result.text[0] == c);
}

/** Require exact bytes, an exact fit, and no write for a short buffer. */
static void bytes(unsigned int modes, const char *s, unsigned int n)
{
    char buf[KM_FUNC_BYTES + 16];
    unsigned int i, got;

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (char)0x55;
    }
    got = keymap_encode(&map, &result, modes, buf, n);
    CHECK(got == n);
    for (i = 0; i < n; i++) {
        CHECK(buf[i] == s[i]);
    }
    CHECK(buf[n] == (char)0x55);
    if (n) {
        for (i = 0; i < sizeof(buf); i++) {
            buf[i] = (char)0x55;
        }
        CHECK(keymap_encode(&map, &result, modes, buf, n - 1) == 0);
        for (i = 0; i < sizeof(buf); i++) {
            CHECK(buf[i] == (char)0x55);
        }
    }
}

/** Check US text, Enter, both modifier sides, repeats and invalid events. */
static void us(void)
{
    reset();
    tap(30);
    point('a');
    bytes(0, "a", 1);
    event(42, 1);
    event(54, 1);
    event(42, 0);
    CHECK(state.down[KM_G_SHIFT] == 1);
    tap(30);
    point('A');
    event(54, 2);
    CHECK(state.down[KM_G_SHIFT] == 1);
    event(54, 0);
    CHECK(state.shift == 0);
    event(29, 1);
    event(97, 1);
    event(29, 0);
    tap(46);
    point(3);
    bytes(0, "\003", 1);
    event(97, 0);
    event(56, 1);
    tap(30);
    bytes(0, "\033a", 2);
    event(56, 0);
    event(100, 1);
    tap(30);
    bytes(0, "\033a", 2);
    event(100, 0);
    tap(28);
    bytes(0, "\r", 1);
    tap(14);
    bytes(0, "\177", 1);
    event(42, 1);
    tap(15);
    bytes(0, "\033[Z", 3);
    event(42, 0);
    event(30, 1);
    point('a');
    event(30, 2);
    point('a');
    CHECK(result.repeat);
    event(30, 0);
    CHECK(event(30, 2) == KM_RESULT_NONE);
    CHECK(event(256, 1) == KM_RESULT_NONE);
    CHECK(event(30, -1) == KM_RESULT_NONE);
    CHECK(event(30, 3) == KM_RESULT_NONE);
    CHECK(entry(0, 256, 30) == KM_NOSUCHMAP);
    CHECK(keymap_default_entry(keymap_default_tables, 255, 0) == KM_NOSUCHMAP);
    CHECK(keymap_default_entry(keymap_default_tables, 0, 256) == KM_HOLE);
    CHECK(keymap_default_entry(0, 0, 30) == KM_NOSUCHMAP);
}

/** Check two-layout ShiftL locks and Ctrl using the first layout's table. */
static void layouts(void)
{
    reset();
    load("0 42 0700\n0 56 0703\n1 56 0a04\n8 42 0a04\n"
         "10 42 0700\n10 56 0703\n11 56 0a04\n18 42 0a04\n"
         "10 46 f441\n11 46 f421\n10 58 0207\n11 58 0207\n"
         "10 29 0702\n14 46 0003\n14 29 0702\n");
    event(56, 1);
    tap(42);
    event(56, 0);
    CHECK(state.locks == 16 && state.shift == 0);
    tap(46);
    point(0x441);
    bytes(0, "\321\201", 2);
    CHECK(result.mods == 0);
    tap(58);
    CHECK(state.ledflags == KM_LED_CAPS);
    tap(46);
    point(0x421);
    bytes(0, "\320\241", 2);
    event(42, 1);
    tap(46);
    point(0x441);
    event(42, 0);
    event(29, 1);
    tap(46);
    point(3);
    bytes(0, "\003", 1);
    event(29, 0);
    event(42, 1);
    tap(56);
    event(42, 0);
    CHECK(state.locks == 0 && state.shift == 0);
    tap(46);
    point('C');
    reset();
    load("0 16 0027\n0 17 002c\n0 18 002e\n0 19 0070\n");
    tap(16);
    point('\'');
    tap(17);
    point(',');
    tap(18);
    point('.');
    tap(19);
    point('p');
    reset();
    load("0 18 0066\n0 19 0070\n0 20 0067\n0 21 006a\n");
    tap(18);
    point('f');
    tap(19);
    point('p');
    tap(20);
    point('g');
    tap(21);
    point('j');
}

/** Check dead keys, Compose, failure pairs, cancellation and live accents. */
static void compose(void)
{
    struct keymap_diacr custom = {'^', 'a', 0x1234};

    reset();
    load("0 13 0401\n0 41 0402\n0 127 020e\n");
    tap(13);
    CHECK(result.kind == KM_RESULT_NONE);
    tap(18);
    point(0xE9);
    bytes(0, "\303\251", 2);
    tap(41);
    tap(30);
    point(0xE2);
    tap(41);
    tap(57);
    point('^');
    tap(41);
    tap(41);
    point('^');
    tap(41);
    tap(48);
    CHECK(result.kind == KM_RESULT_TEXT && result.ntext == 2);
    CHECK(result.text[0] == '^' && result.text[1] == 'b');
    bytes(0, "^b", 2);
    tap(127);
    tap(40);
    CHECK(result.kind == KM_RESULT_NONE);
    tap(18);
    point(0xE9);
    tap(127);
    tap(30);
    tap(1);
    point(27);
    CHECK(!state.compose && !state.diacr);
    tap(127);
    tap(14);
    CHECK(result.kind == KM_RESULT_NONE && !state.compose);
    tap(41);
    tap(14);
    CHECK(result.kind == KM_RESULT_NONE && !state.diacr);
    tap(41);
    tap(1);
    bytes(0, "\033", 1);
    CHECK(!state.diacr);
    tap(127);
    tap(127);
    tap(30);
    CHECK(state.diacr == 'a');
    tap(14);
    map.diacr = &custom;
    map.ndiacr = 1;
    tap(41);
    tap(30);
    point(0x1234);
    map.diacr = keymap_default_diacr;
    map.ndiacr = keymap_default_ndiacr;
    load("0 13 0d22\n");
    tap(13);
    tap(22);
    point(0xFC);
    CHECK(keymap_default_ndiacr > 0 && keymap_default_ndiacr <= KM_MAX_DIACR);
}

/** Check Caps Lock for nonletters and case pairs outside the Latin script. */
static void caps(void)
{
    reset();
    tap(58);
    tap(2);
    point('1');
    event(58, 1);
    event(58, 2);
    CHECK(state.ledflags == 0);
    event(58, 0);
    load("0 30 f4e9\n1 30 f4e8\n");
    tap(58);
    tap(30);
    point(0x4E8);
    event(42, 1);
    tap(30);
    point(0x4E9);
    event(42, 0);
    load("0 30 f561\n1 30 f531\n");
    tap(30);
    point(0x531);
    load("0 30 e0d0\n1 30 ec90\n");
    tap(30);
    point(0x1C90);
    load("0 30 005b\n1 30 007b\n");
    tap(30);
    point('[');
    load("0 30 f431\n1 30 f42f\n");
    tap(30);
    point(0x431);
    load("0 30 f3c2\n1 30 f3c3\n");
    tap(30);
    point(0x3C2);
}

/** Check sticky modifiers, missing tables, reloaded keys and Alt numbers. */
static void state_changes(void)
{
    reset();
    load("0 42 0c00\n1 42 0c00\n");
    tap(42);
    CHECK(state.slocks == 1 && state.shift == 0);
    tap(30);
    point('A');
    CHECK(state.slocks == 0);
    tap(30);
    point('a');
    tap(42);
    tap(28);
    CHECK(state.slocks == 0);
    tap(30);
    point('a');
    tap(42);
    tap(42);
    CHECK(state.slocks == 0);
    event(42, 1);
    tap(30);
    point('A');
    event(42, 0);
    tap(30);
    point('a');
    reset();
    event(42, 1);
    load("1 42 0a04\n");
    event(42, 0);
    CHECK(state.shift == 0);
    reset();
    load("0 42 0704\n");
    event(42, 1);
    CHECK(event(30, 1) == KM_RESULT_NONE);
    event(30, 0);
    event(42, 0);
    CHECK(state.shift == 0);
    reset();
    load("8 79 0900\n8 80 0906\n8 81 0905\n");
    event(56, 1);
    tap(80);
    tap(81);
    event(56, 0);
    point('A');
    bytes(0, "A", 1);
    event(56, 1);
    tap(79);
    event(56, 0);
    point(0);
    bytes(0, "\0", 1);
    reset();
    load("0 42 0708\n");
    tap(58);
    event(42, 1);
    CHECK(state.ledflags == 0 && state.shift == 1);
    event(42, 0);
    CHECK(state.shift == 0);
    keymap_state_init(&state, 0xFF);
    CHECK(state.ledflags == 7 && !state.compose);
}

/** Check that Linux's console switches send the plain key with Alt, and
 * that the held modifiers are rebuilt from the keys down. */
static void foreign(void)
{
    static unsigned char held[KM_NR_KEYS / 8];

    reset();
    load("8 59 0500\n8 105 0210\n8 103 0212\n0 84 0206\n8 84 0206\n");
    event(56, 1);
    tap(59);
    CHECK(result.kind == KM_RESULT_FUNC && result.func == 0);
    bytes(0, "\033[1;3P", 6);
    tap(105);
    bytes(0, "\033[1;3D", 6);
    tap(103);
    bytes(0, "\033[1;3A", 6);
    CHECK(event(84, 1) == KM_RESULT_NONE);
    event(84, 0);
    event(56, 0);
    reset();
    event(42, 1);
    keymap_state_held(&state, &map, held);
    CHECK(state.shift == 0);
    tap(30);
    point('a');
    held[29 >> 3] |= 1U << (29 & 7);
    keymap_state_held(&state, &map, held);
    CHECK(state.shift == 1U << KM_G_CTRL);
    event(29, 2);
    CHECK(result.kind == KM_RESULT_NONE);
    event(29, 0);
    CHECK(state.shift == 0);
}

/** Check literal functions, terminal modes, modifiers, UTF-8 and bounds. */
static void encoding(void)
{
    static const char *const functions[KM_MAX_FUNC] = {"hello", "\033[1;5A"};
    static const unsigned int keys[] = {103, 108, 106, 105, 102, 107, 110, 111, 104, 109, 59,
                                        60,  61,  62,  63,  64,  65,  66,  67,  68,  87,  88};
    static const char *const plain[] = {"\033[A",   "\033[B",   "\033[C",   "\033[D",   "\033[H",
                                        "\033[F",   "\033[2~",  "\033[3~",  "\033[5~",  "\033[6~",
                                        "\033OP",   "\033OQ",   "\033OR",   "\033OS",   "\033[15~",
                                        "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~",
                                        "\033[23~", "\033[24~"};
    static const char *const modified[] = {
        "\033[1;8A",  "\033[1;8B",  "\033[1;8C",  "\033[1;8D",  "\033[1;8H",  "\033[1;8F",
        "\033[2;8~",  "\033[3;8~",  "\033[5;8~",  "\033[6;8~",  "\033[1;8P",  "\033[1;8Q",
        "\033[1;8R",  "\033[1;8S",  "\033[15;8~", "\033[17;8~", "\033[18;8~", "\033[19;8~",
        "\033[20;8~", "\033[21;8~", "\033[23;8~", "\033[24;8~"};
    static const unsigned long ctrl[] = {'@', '[', '\\', ']', '^', '_', ' '};
    static const char codes[] = {0, 27, 28, 29, 30, 31, 0};
    unsigned int i, n;
    char buf[4];

    reset();
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        tap(keys[i]);
        for (n = 0; plain[i][n]; n++) {
        }
        bytes(0, plain[i], n);
        result.mods = 7;
        if (i == 8 || i == 9) {
            bytes(0, "", 0);
            continue;
        }
        for (n = 0; modified[i][n]; n++) {
        }
        bytes(KM_MODE_CURSOR, modified[i], n);
    }
    event(42, 1);
    event(29, 1);
    tap(23);
    bytes(0, "\t", 1);
    tap(15);
    bytes(0, "\033[Z", 3);
    tap(41);
    bytes(0, "\036", 1);
    tap(26);
    bytes(0, "\033", 1);
    tap(104);
    CHECK(result.key == KM_SCROLLBACK);
    bytes(0, "", 0);
    event(29, 0);
    event(56, 1);
    tap(15);
    bytes(0, "\033[Z", 3);
    event(56, 0);
    event(42, 0);
    tap(86);
    bytes(0, "<", 1);
    tap(127);
    bytes(0, "\033[29~", 5);
    tap(103);
    bytes(KM_MODE_CURSOR, "\033OA", 3);
    tap(102);
    bytes(KM_MODE_CURSOR, "\033OH", 3);
    tap(107);
    bytes(KM_MODE_CURSOR, "\033OF", 3);
    result.kind = KM_RESULT_TEXT;
    result.ntext = 1;
    result.mods = KM_MOD_CTRL;
    for (i = 0; i < sizeof(ctrl) / sizeof(ctrl[0]); i++) {
        result.text[0] = ctrl[i];
        bytes(0, codes + i, 1);
    }
    result.mods = KM_MOD_ALT | KM_MOD_CTRL;
    result.text[0] = '[';
    bytes(0, "\033\033", 2);
    result.mods = 0;
    result.text[0] = 0x1F600;
    bytes(0, "\360\237\230\200", 4);
    result.text[0] = 0xD800;
    CHECK(keymap_encode(&map, &result, 0, buf, 4) == 0);
    result.text[0] = 0x110000;
    CHECK(keymap_encode(&map, &result, 0, buf, 4) == 0);
    result.kind = KM_RESULT_FUNC;
    result.func = 0;
    result.mods = 7;
    map.func = functions;
    bytes(0, "hello", 5);
    result.func = 1;
    bytes(0, "\033[1;5A", 6);
    result.func = 256;
    CHECK(keymap_encode(&map, &result, 0, buf, 4) == 0);
    result.kind = KM_RESULT_TEXT;
    result.ntext = 3;
    CHECK(keymap_encode(&map, &result, 0, buf, 4) == 0);
    CHECK(keymap_encode(&map, &result, 0, 0, 4) == 0);
}

/** Check keypad navigation, numeric reversal and application sequences. */
static void pad(void)
{
    reset();
    tap(71);
    bytes(0, "\033[H", 3);
    bytes(KM_MODE_CURSOR, "\033OH", 3);
    tap(82);
    bytes(KM_MODE_CURSOR, "\033[2~", 4);
    tap(83);
    bytes(0, "\033[3~", 4);
    tap(79);
    bytes(KM_MODE_KEYPAD, "\033[F", 3);
    event(42, 1);
    tap(79);
    bytes(KM_MODE_KEYPAD, "\033O2q", 4);
    result.mods |= KM_MOD_CTRL;
    bytes(KM_MODE_KEYPAD, "\033O6q", 4);
    event(42, 0);
    tap(96);
    bytes(KM_MODE_KEYPAD, "\033OM", 3);
    bytes(0, "\r", 1);
    tap(55);
    bytes(KM_MODE_KEYPAD, "\033Oj", 3);
    bytes(0, "*", 1);
    tap(69);
    CHECK(state.ledflags & KM_LED_NUM);
    tap(79);
    bytes(0, "1", 1);
    bytes(KM_MODE_KEYPAD, "1", 1);
    result.mods = KM_MOD_CTRL | KM_MOD_ALT;
    bytes(KM_MODE_KEYPAD, "1", 1);
    event(29, 1);
    event(56, 1);
    tap(98);
    bytes(0, "/", 1);
    bytes(KM_MODE_KEYPAD, "/", 1);
    event(56, 0);
    event(29, 0);
    event(42, 1);
    tap(71);
    bytes(0, "\033[1;2H", 6);
    event(42, 0);
    tap(69);
    event(42, 1);
    tap(71);
    bytes(0, "7", 1);
    event(42, 0);
}

/** Run the pure translator checks; no files or processes need cleanup. */
int main(void)
{
    us();
    layouts();
    compose();
    caps();
    state_changes();
    foreign();
    encoding();
    pad();
    printf("keymap-test: result: %d failures\n", fails);
    return fails ? 1 : 0;
}
