/*
 * keytable.c: the reader of keytable.txt, which turns each line of the table
 * into the cases of key events it stands for.
 */

#include "keytable.h"

#include <string.h>

/* The 105 keys of a PC keyboard with Linux's names and numbers. */
const struct keytable_key keytable_keys[] = {
    {"esc", 1},       {"1", 2},           {"2", 3},           {"3", 4},
    {"4", 5},         {"5", 6},           {"6", 7},           {"7", 8},
    {"8", 9},         {"9", 10},          {"0", 11},          {"minus", 12},
    {"equal", 13},    {"backspace", 14},  {"tab", 15},        {"q", 16},
    {"w", 17},        {"e", 18},          {"r", 19},          {"t", 20},
    {"y", 21},        {"u", 22},          {"i", 23},          {"o", 24},
    {"p", 25},        {"leftbrace", 26},  {"rightbrace", 27}, {"enter", 28},
    {"leftctrl", 29}, {"a", 30},          {"s", 31},          {"d", 32},
    {"f", 33},        {"g", 34},          {"h", 35},          {"j", 36},
    {"k", 37},        {"l", 38},          {"semicolon", 39},  {"apostrophe", 40},
    {"grave", 41},    {"leftshift", 42},  {"backslash", 43},  {"z", 44},
    {"x", 45},        {"c", 46},          {"v", 47},          {"b", 48},
    {"n", 49},        {"m", 50},          {"comma", 51},      {"dot", 52},
    {"slash", 53},    {"rightshift", 54}, {"kpasterisk", 55}, {"leftalt", 56},
    {"space", 57},    {"capslock", 58},   {"f1", 59},         {"f2", 60},
    {"f3", 61},       {"f4", 62},         {"f5", 63},         {"f6", 64},
    {"f7", 65},       {"f8", 66},         {"f9", 67},         {"f10", 68},
    {"numlock", 69},  {"scrolllock", 70}, {"kp7", 71},        {"kp8", 72},
    {"kp9", 73},      {"kpminus", 74},    {"kp4", 75},        {"kp5", 76},
    {"kp6", 77},      {"kpplus", 78},     {"kp1", 79},        {"kp2", 80},
    {"kp3", 81},      {"kp0", 82},        {"kpdot", 83},      {"102nd", 86},
    {"f11", 87},      {"f12", 88},        {"kpenter", 96},    {"rightctrl", 97},
    {"kpslash", 98},  {"sysrq", 99},      {"rightalt", 100},  {"home", 102},
    {"up", 103},      {"pageup", 104},    {"left", 105},      {"right", 106},
    {"end", 107},     {"down", 108},      {"pagedown", 109},  {"insert", 110},
    {"delete", 111},  {"pause", 119},     {"leftmeta", 125},  {"rightmeta", 126},
    {"compose", 127},
};

const int keytable_nkeys = (int)(sizeof(keytable_keys) / sizeof(keytable_keys[0]));

/* The left and right keys of the classes shift, ctrl and alt. */
static const unsigned short class_keys[3][2] = {{42, 54}, {29, 97}, {56, 100}};
static const char *const class_names[3] = {"shift", "ctrl", "alt"};

/* A token of a line: a word, or a quoted string with its bytes. */
struct token {
    char text[64];
    int quoted;
    int nbytes;
    unsigned char bytes[KEYTABLE_MAX_BYTES];
};

int keytable_key_code(const char *name)
{
    int i;

    for (i = 0; i < keytable_nkeys; i++) {
        if (strcmp(keytable_keys[i].name, name) == 0) {
            return keytable_keys[i].code;
        }
    }
    return -1;
}

const char *keytable_key_name(unsigned code)
{
    int i;

    for (i = 0; i < keytable_nkeys; i++) {
        if (keytable_keys[i].code == code) {
            return keytable_keys[i].name;
        }
    }
    return NULL;
}

/** Return the value of the hexadecimal digit `c`, or -1. */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** Record a refusal of the current line in t->error and return -1. */
static int refuse(struct keytable *t, const char *why, const char *what)
{
    sprintf(t->error, "%.60s:%d: %.60s%s%.60s", t->name, t->line, why, what[0] ? ": " : "", what);
    return -1;
}

/**
 * Decode the escape after the backslash at `*s`, leaving `*s` at its last
 * character. Return the byte, or -1 for a malformed escape, with the reason
 * in t->error.
 */
static int string_escape(struct keytable *t, const char **s)
{
    const char *e = *s;

    switch (*e) {
    case 'e':
        return 0x1b;
    case 'r':
        return '\r';
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'b':
        return '\b';
    case '\\':
    case '"':
        return *e;
    case 'x':
        if (hexval(e[1]) < 0 || hexval(e[2]) < 0) {
            return refuse(t, "\\x wants two hexadecimal digits", "");
        }
        *s = e + 2;
        return hexval(e[1]) * 16 + hexval(e[2]);
    default:
        return refuse(t, "unknown escape in a string", "");
    }
}

/**
 * Read the quoted string whose opening quote is at `*p` into `tok`, with its
 * escapes decoded, and leave `*p` past the closing quote. Return 1, or -1
 * for a malformed string, with the reason in t->error.
 */
static int string_token(struct keytable *t, const char **p, struct token *tok)
{
    const char *s = *p;

    tok->quoted = 1;
    tok->text[0] = '\0';
    for (s++; *s != '"'; s++) {
        int c = (unsigned char)*s;

        if (c == '\0' || c == '\n') {
            return refuse(t, "unterminated string", "");
        }
        if (c < 0x20 || c > 0x7e) {
            return refuse(t, "a control or non-ASCII byte in a string", "");
        }
        if (c == '\\') {
            s++;
            c = string_escape(t, &s);
            if (c < 0) {
                return -1;
            }
        }
        if (tok->nbytes >= KEYTABLE_MAX_BYTES) {
            return refuse(t, "string too long", "");
        }
        tok->bytes[tok->nbytes++] = (unsigned char)c;
    }
    *p = s + 1;
    return 1;
}

/**
 * Read the next token of the line at `*p` into `tok`, past the opening quote's
 * escapes for a quoted one.
 *
 * Return 1 for a token, 0 at the end of the line or a comment, and -1 for a
 * malformed one, with the reason in t->error.
 */
static int next_token(struct keytable *t, const char **p, struct token *tok)
{
    const char *s = *p;
    size_t n = 0;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    if (*s == '\0' || *s == '#') {
        *p = s;
        return 0;
    }
    tok->quoted = 0;
    tok->nbytes = 0;
    if (*s != '"') {
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
            if (n + 1 >= sizeof(tok->text)) {
                return refuse(t, "word too long", "");
            }
            tok->text[n++] = *s++;
        }
        tok->text[n] = '\0';
        *p = s;
        return 1;
    }
    if (string_token(t, &s, tok) < 0) {
        return -1;
    }
    if (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        return refuse(t, "text after a closing quote", "");
    }
    *p = s;
    return 1;
}

/** Set the result of `tp` from `tok`, or return -1 with the reason. */
static int set_result(struct keytable *t, struct keytable_template *tp, const struct token *tok)
{
    tp->kind = KEYTABLE_BYTES;
    tp->sig = 0;
    tp->nbytes = 0;
    if (tok->quoted) {
        tp->nbytes = tok->nbytes;
        memcpy(tp->bytes, tok->bytes, (size_t)tok->nbytes);
    } else if (strcmp(tok->text, "local") == 0) {
        tp->kind = KEYTABLE_LOCAL;
    } else if (strcmp(tok->text, "SIGINT") == 0) {
        tp->kind = KEYTABLE_SIGNAL;
        tp->sig = 2;
    } else if (strcmp(tok->text, "SIGQUIT") == 0) {
        tp->kind = KEYTABLE_SIGNAL;
        tp->sig = 3;
    } else if (strcmp(tok->text, "SIGTSTP") == 0) {
        tp->kind = KEYTABLE_SIGNAL;
        tp->sig = 20;
    } else {
        return refuse(t, "not a result", tok->text);
    }
    if (tp->kind == KEYTABLE_SIGNAL && !(t->modes & KEYTABLE_TTY)) {
        return refuse(t, "a signal without the tty mode", tok->text);
    }
    return 0;
}

/** Append the template event to `tp`, or return -1 when it is full. */
static int add_tevent(struct keytable *t, struct keytable_template *tp, int code, int cls, int what)
{
    if (tp->nevents >= KEYTABLE_MAX_EVENTS) {
        return refuse(t, "too many events", "");
    }
    tp->events[tp->nevents].code = (short)code;
    tp->events[tp->nevents].cls = (signed char)cls;
    tp->events[tp->nevents].what = (char)what;
    tp->nevents++;
    return 0;
}

/** Parse the modes line's list at `*p` into t->modes. */
static int parse_modes(struct keytable *t, const char *p)
{
    static const struct {
        const char *name;
        unsigned bit;
    } modes[] = {
        {"ckm", KEYTABLE_CKM},     {"kpam", KEYTABLE_KPAM}, {"num", KEYTABLE_NUM},
        {"caps", KEYTABLE_CAPS},   {"tty", KEYTABLE_TTY},   {"icanon", KEYTABLE_ICANON},
        {"icrnl", KEYTABLE_ICRNL}, {"isig", KEYTABLE_ISIG},
    };
    struct token tok;
    char *word;
    unsigned m = 0;
    size_t i;
    int r;

    if ((r = next_token(t, &p, &tok)) <= 0 || tok.quoted) {
        return r < 0 ? r : refuse(t, "modes wants a list", "");
    }
    if (strcmp(tok.text, "-") != 0) {
        for (word = strtok(tok.text, ","); word; word = strtok(NULL, ",")) {
            for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
                if (strcmp(modes[i].name, word) == 0) {
                    break;
                }
            }
            if (i == sizeof(modes) / sizeof(modes[0])) {
                return refuse(t, "unknown mode", word);
            }
            m |= modes[i].bit;
        }
    }
    if ((m & (KEYTABLE_ICANON | KEYTABLE_ICRNL | KEYTABLE_ISIG)) && !(m & KEYTABLE_TTY)) {
        return refuse(t, "a termios flag without tty", "");
    }
    if ((r = next_token(t, &p, &tok)) != 0) {
        return r < 0 ? r : refuse(t, "text after the modes", tok.text);
    }
    t->modes = m;
    return 0;
}

/** Parse a press line's events and result at `p` into one template. */
static int parse_press(struct keytable *t, const char *p)
{
    struct keytable_template *tp = &t->tmpl[0];
    struct token tok;
    int r, code, cls;
    const char *name;
    char what;

    tp->nevents = 0;
    for (;;) {
        if ((r = next_token(t, &p, &tok)) <= 0) {
            return r < 0 ? r : refuse(t, "press wants = and a result", "");
        }
        if (tok.quoted) {
            return refuse(t, "a string among the events", "");
        }
        if (strcmp(tok.text, "=") == 0) {
            break;
        }
        what = 't';
        name = tok.text;
        if (name[0] == '+' || name[0] == '-') {
            what = name[0];
            name++;
        }
        code = keytable_key_code(name);
        for (cls = 0; code < 0 && cls < 3; cls++) {
            if (strcmp(class_names[cls], name) == 0) {
                break;
            }
        }
        if (code < 0 && cls == 3) {
            return refuse(t, "unknown key", name);
        }
        if (add_tevent(t, tp, code, code < 0 ? cls : -1, what) < 0) {
            return -1;
        }
    }
    if (tp->nevents == 0) {
        return refuse(t, "press wants events", "");
    }
    if ((r = next_token(t, &p, &tok)) <= 0) {
        return r < 0 ? r : refuse(t, "press wants a result", "");
    }
    if (set_result(t, tp, &tok) < 0) {
        return -1;
    }
    if ((r = next_token(t, &p, &tok)) != 0) {
        return r < 0 ? r : refuse(t, "text after the result", tok.quoted ? "" : tok.text);
    }
    t->ntmpl = 1;
    return 0;
}

/**
 * Parse a key line of `code` and its eight cells at `p` into eight templates,
 * column j holding Shift when bit 0 of j is set, Ctrl for bit 1 and Alt for
 * bit 2.
 */
static int parse_key(struct keytable *t, int code, const char *p)
{
    struct token tok;
    int j, cls, r;

    for (j = 0; j < 8; j++) {
        struct keytable_template *tp = &t->tmpl[j];

        if ((r = next_token(t, &p, &tok)) <= 0) {
            return r < 0 ? r : refuse(t, "a key line wants eight cells", "");
        }
        tp->nevents = 0;
        for (cls = 0; cls < 3; cls++) {
            if (j & (1 << cls)) {
                add_tevent(t, tp, -1, cls, '+');
            }
        }
        add_tevent(t, tp, code, -1, 't');
        for (cls = 2; cls >= 0; cls--) {
            if (j & (1 << cls)) {
                add_tevent(t, tp, -1, cls, '-');
            }
        }
        if (set_result(t, tp, &tok) < 0) {
            return -1;
        }
    }
    if ((r = next_token(t, &p, &tok)) != 0) {
        return r < 0 ? r : refuse(t, "a key line wants eight cells", "");
    }
    t->ntmpl = 8;
    return 0;
}

/** Return the number of side choices of `tp`: 3 to the classes it uses. */
static unsigned count_variants(const struct keytable_template *tp)
{
    unsigned used = 0, n = 1;
    int i;

    for (i = 0; i < tp->nevents; i++) {
        if (tp->events[i].code < 0) {
            used |= 1u << tp->events[i].cls;
        }
    }
    for (i = 0; i < 3; i++) {
        if (used & (1u << i)) {
            n *= 3;
        }
    }
    return n;
}

/** Append the event of `code` and `press` to `c`, or return -1 when full. */
static int add_event(struct keytable *t, struct keytable_case *c, unsigned code, int press)
{
    if (c->nevents >= KEYTABLE_MAX_EVENTS) {
        return refuse(t, "too many events", "");
    }
    c->events[c->nevents].code = (unsigned short)code;
    c->events[c->nevents].press = (unsigned char)press;
    c->nevents++;
    return 0;
}

/**
 * Make case `c` from template `tp` with side choice `variant`: a digit in base
 * 3 for each class the template uses, in the order shift, ctrl and alt, of 0
 * for the left key, 1 for the right and 2 for both.
 */
static int resolve(struct keytable *t, const struct keytable_template *tp, unsigned variant,
                   struct keytable_case *c)
{
    int side[3], i, k, r = 0;
    unsigned used = 0;

    for (i = 0; i < tp->nevents; i++) {
        if (tp->events[i].code < 0) {
            used |= 1u << tp->events[i].cls;
        }
    }
    for (i = 0; i < 3; i++) {
        side[i] = 0;
        if (used & (1u << i)) {
            side[i] = (int)(variant % 3);
            variant /= 3;
        }
    }
    c->nevents = 0;
    for (i = 0; i < tp->nevents && r == 0; i++) {
        const struct keytable_tevent *e = &tp->events[i];
        unsigned keys[2];
        int nkeys = 1;

        if (e->code >= 0) {
            keys[0] = (unsigned)e->code;
        } else if (side[(int)e->cls] < 2) {
            keys[0] = class_keys[(int)e->cls][side[(int)e->cls]];
        } else {
            keys[0] = class_keys[(int)e->cls][0];
            keys[1] = class_keys[(int)e->cls][1];
            nkeys = 2;
        }
        if (e->what == '+' || e->what == 't') {
            for (k = 0; k < nkeys && r == 0; k++) {
                r = add_event(t, c, keys[k], 1);
            }
        }
        if (e->what == '-' || e->what == 't') {
            for (k = nkeys - 1; k >= 0 && r == 0; k--) {
                r = add_event(t, c, keys[k], 0);
            }
        }
    }
    c->modes = t->modes;
    c->kind = tp->kind;
    c->sig = tp->sig;
    c->nbytes = tp->nbytes;
    memcpy(c->bytes, tp->bytes, (size_t)tp->nbytes);
    c->line = t->tline;
    return r;
}

/** Read and parse lines until one gives templates; return 1, 0 at the end
 * or -1. */
static int read_line(struct keytable *t)
{
    char buf[1024];
    const char *p;
    struct token tok;
    int r, code;

    while (fgets(buf, sizeof(buf), t->f)) {
        t->line++;
        if (!strchr(buf, '\n') && !feof(t->f)) {
            return refuse(t, "line too long", "");
        }
        p = buf;
        if ((r = next_token(t, &p, &tok)) < 0) {
            return -1;
        }
        if (r == 0) {
            continue;
        }
        if (tok.quoted) {
            return refuse(t, "a line begins with a string", "");
        }
        if (strcmp(tok.text, "modes") == 0) {
            if (parse_modes(t, p) < 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(tok.text, "press") == 0) {
            r = parse_press(t, p);
        } else if ((code = keytable_key_code(tok.text)) >= 0) {
            r = parse_key(t, code, p);
        } else {
            return refuse(t, "unknown key", tok.text);
        }
        if (r < 0) {
            return -1;
        }
        t->tline = t->line;
        t->ti = 0;
        t->variant = 0;
        t->nvariants = count_variants(&t->tmpl[0]);
        return 1;
    }
    if (ferror(t->f)) {
        return refuse(t, "read error", "");
    }
    return 0;
}

void keytable_init(struct keytable *t, FILE *f, const char *name)
{
    memset(t, 0, sizeof(*t));
    t->f = f;
    t->name = name;
}

int keytable_open(struct keytable *t, const char *path)
{
    FILE *f = fopen(path, "r");

    keytable_init(t, f, path);
    if (!f) {
        sprintf(t->error, "%.150s: cannot open", path);
        return -1;
    }
    t->owned = 1;
    return 0;
}

int keytable_next(struct keytable *t, struct keytable_case *c)
{
    int r;

    while (t->ti >= t->ntmpl || t->variant >= t->nvariants) {
        if (t->ti < t->ntmpl && ++t->ti < t->ntmpl) {
            t->variant = 0;
            t->nvariants = count_variants(&t->tmpl[t->ti]);
            continue;
        }
        if ((r = read_line(t)) <= 0) {
            t->ntmpl = 0;
            return r;
        }
    }
    if (resolve(t, &t->tmpl[t->ti], t->variant, c) < 0) {
        return -1;
    }
    t->variant++;
    return 1;
}

void keytable_close(struct keytable *t)
{
    if (t->owned && t->f) {
        fclose(t->f);
    }
    t->f = NULL;
    t->owned = 0;
}

void keytable_quote(const unsigned char *bytes, int n, char *buf, size_t size)
{
    size_t len = 0;
    char piece[8];
    int i;

    if (size == 0) {
        return;
    }
    buf[0] = '\0';
    for (i = -1; i <= n; i++) {
        int c = i >= 0 && i < n ? bytes[i] : '"';

        if (i < 0 || i == n) {
            strcpy(piece, "\"");
        } else if (c == 0x1b) {
            strcpy(piece, "\\e");
        } else if (c == '\r') {
            strcpy(piece, "\\r");
        } else if (c == '\n') {
            strcpy(piece, "\\n");
        } else if (c == '\t') {
            strcpy(piece, "\\t");
        } else if (c == '\b') {
            strcpy(piece, "\\b");
        } else if (c == '\\' || c == '"') {
            piece[0] = '\\';
            piece[1] = (char)c;
            piece[2] = '\0';
        } else if (c < 0x20 || c > 0x7e) {
            sprintf(piece, "\\x%02x", (unsigned)c);
        } else {
            piece[0] = (char)c;
            piece[1] = '\0';
        }
        if (len + strlen(piece) + 1 > size) {
            break;
        }
        strcpy(buf + len, piece);
        len += strlen(piece);
    }
}

void keytable_describe(const struct keytable_case *c, char *buf, size_t size)
{
    static const char *const modes[] = {"ckm", "kpam",   "num",   "caps",
                                        "tty", "icanon", "icrnl", "isig"};
    char text[512], result[300];
    size_t len = 0;
    int i, any = 0;

    len += (size_t)sprintf(text, "line %d, modes ", c->line);
    for (i = 0; i < 8; i++) {
        if (c->modes & (1u << i)) {
            len += (size_t)sprintf(text + len, "%s%s", any ? "," : "", modes[i]);
            any = 1;
        }
    }
    if (!any) {
        text[len++] = '-';
    }
    text[len++] = ':';
    for (i = 0; i < c->nevents && len < 400; i++) {
        const char *name = keytable_key_name(c->events[i].code);

        len += (size_t)sprintf(text + len, " %c%.20s", c->events[i].press ? '+' : '-',
                               name ? name : "?");
    }
    if (c->kind == KEYTABLE_LOCAL) {
        strcpy(result, "local");
    } else if (c->kind == KEYTABLE_SIGNAL) {
        strcpy(result, c->sig == 2 ? "SIGINT" : c->sig == 3 ? "SIGQUIT" : "SIGTSTP");
    } else {
        keytable_quote(c->bytes, c->nbytes, result, sizeof(result));
    }
    sprintf(text + len, " = %.80s", result);
    if (size > 0) {
        strncpy(buf, text, size - 1);
        buf[size - 1] = '\0';
    }
}
