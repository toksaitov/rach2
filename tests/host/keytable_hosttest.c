/*
 * keytable_hosttest.c: the key table, tests/host/keytable.txt, and its
 * reader, tests/host/keytable.c.
 *
 * host-deps: tests/host/keytable.c tests/host/compat/fixdir_host.c
 * host-inputs: tests/host/keytable.txt
 *
 * The table must read without a refusal, name each of the 105 keys in some
 * case, and give a result for every case that the reader's quoting writes and
 * reads back unchanged. Small tables written to the test's fixture directory
 * check the expansion: a key line is 64 cases, with the modifiers pressed in
 * the order Shift, Ctrl and Alt and released in reverse, a class on both sides
 * pressed left first and released right first, and a press line with one
 * class is 3; and the reader refuses each malformed line, naming its line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixdir_host.h"
#include "keytable.h"

static int failures;

#define CHECK(cond, what)                                           \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "keytable_hosttest: fail: %s\n", what); \
            failures++;                                             \
        }                                                           \
    } while (0)

/** Write `text` to the fixture `name` and open it as a table in `t`. */
static void open_text(struct keytable *t, const char *name, const char *text)
{
    const char *path = fix_path(name);
    FILE *f = fopen(path, "w");

    if (!f || fputs(text, f) == EOF || fclose(f) != 0) {
        fprintf(stderr, "keytable_hosttest: cannot write %s\n", path);
        exit(1);
    }
    if (keytable_open(t, path) < 0) {
        fprintf(stderr, "keytable_hosttest: %s\n", t->error);
        exit(1);
    }
}

/** Say whether case `c` has exactly the events of `codes`, a press for a
 * positive code and a release for a negative one. */
static int events_are(const struct keytable_case *c, const int *codes, int n)
{
    int i;

    if (c->nevents != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        int code = codes[i] < 0 ? -codes[i] : codes[i];

        if (c->events[i].code != code || c->events[i].press != (codes[i] > 0)) {
            return 0;
        }
    }
    return 1;
}

/** Read the real table: no refusal, every key named, results that quote back. */
static void check_table(void)
{
    static unsigned char seen[256];
    struct keytable t, back;
    struct keytable_case c, b;
    const char *path = fix_path("quoted.txt");
    char quoted[300], line[400];
    FILE *f;
    int r, i, n = 0, bytes = 0;

    if (keytable_open(&t, KEYTABLE_PATH) < 0) {
        fprintf(stderr, "keytable_hosttest: %s\n", t.error);
        exit(1);
    }
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "keytable_hosttest: cannot write %s\n", path);
        exit(1);
    }
    while ((r = keytable_next(&t, &c)) == 1) {
        n++;
        for (i = 0; i < c.nevents; i++) {
            seen[c.events[i].code] = 1;
        }
        if (c.kind == KEYTABLE_BYTES) {
            keytable_quote(c.bytes, c.nbytes, quoted, sizeof(quoted));
            fprintf(f, "press a = %s\n", quoted);
            bytes++;
        }
    }
    if (r < 0) {
        fprintf(stderr, "keytable_hosttest: %s\n", t.error);
        failures++;
    }
    keytable_close(&t);
    fclose(f);
    printf("keytable_hosttest: %d cases\n", n);
    CHECK(n > 10000, "the table expands to its cases");
    for (i = 0; i < keytable_nkeys; i++) {
        if (!seen[keytable_keys[i].code]) {
            fprintf(stderr, "keytable_hosttest: fail: key %s in no case\n", keytable_keys[i].name);
            failures++;
        }
    }

    /* Every result, quoted by the reader and read back, is the same bytes. */
    keytable_open(&t, KEYTABLE_PATH);
    if (keytable_open(&back, path) < 0) {
        fprintf(stderr, "keytable_hosttest: %s\n", back.error);
        exit(1);
    }
    n = 0;
    while (keytable_next(&t, &c) == 1) {
        if (c.kind != KEYTABLE_BYTES) {
            continue;
        }
        if (keytable_next(&back, &b) != 1) {
            break;
        }
        n++;
        if (b.nbytes != c.nbytes || memcmp(b.bytes, c.bytes, (size_t)c.nbytes) != 0) {
            keytable_describe(&c, line, sizeof(line));
            fprintf(stderr, "keytable_hosttest: fail: quoted back differently: %s\n", line);
            failures++;
        }
    }
    CHECK(n == bytes, "every quoted result reads back");
    keytable_close(&t);
    keytable_close(&back);
}

/** A key line is 64 cases in column order, sides resolved left, right, both. */
static void check_key_line(void)
{
    static const int first[] = {103, -103};
    static const int shift_right[] = {54, 103, -103, -54};
    static const int all_both[] = {42,   54,   29,  97,  56,  100, 103,
                                   -103, -100, -56, -97, -29, -54, -42};
    static const int per_column[8] = {1, 3, 3, 9, 3, 9, 9, 27};
    struct keytable t;
    struct keytable_case c;
    int col, k, ok = 1, n = 0;
    char line[400];

    open_text(&t, "keyline.txt",
              "# a comment\n"
              "modes ckm,num\n"
              "up \"1\" \"2\" \"3\" \"4\" \"5\" \"6\" local \"\\e[1;8A\" # cells\n");
    for (col = 0; col < 8; col++) {
        for (k = 0; k < per_column[col]; k++) {
            if (keytable_next(&t, &c) != 1) {
                ok = 0;
                break;
            }
            n++;
            if (c.modes != (KEYTABLE_CKM | KEYTABLE_NUM) || c.line != 3) {
                ok = 0;
            }
            if (col == 6 ? c.kind != KEYTABLE_LOCAL : c.kind != KEYTABLE_BYTES) {
                ok = 0;
            }
            if (col < 6 && (c.nbytes != 1 || c.bytes[0] != '1' + col)) {
                ok = 0;
            }
            if (col == 0) {
                CHECK(events_are(&c, first, 2), "the plain column taps the key alone");
            }
            if (col == 1 && k == 1) {
                CHECK(events_are(&c, shift_right, 4),
                      "the second Shift case holds the right Shift");
            }
            if (col == 7 && k == 26) {
                keytable_describe(&c, line, sizeof(line));
                CHECK(events_are(&c, all_both, 14), line);
                CHECK(c.nbytes == 6 && memcmp(c.bytes, "\033[1;8A", 6) == 0,
                      "the last cell's bytes");
            }
        }
    }
    CHECK(ok && n == 64, "a key line is 64 cases with its cells' results");
    CHECK(keytable_next(&t, &c) == 0, "the table ends after its line");
    keytable_close(&t);
}

/** A press line expands its classes, and keeps its keys' own sides. */
static void check_press_line(void)
{
    static const int both[] = {29, 97, 46, -46, -97, -29};
    static const int fixed[] = {42, 54, -42, 30, -30, -54};
    struct keytable t;
    struct keytable_case c;
    int n = 0, last_ok = 0;

    open_text(&t, "press.txt",
              "modes tty,isig\n"
              "press +ctrl c -ctrl = SIGINT\n"
              "modes -\n"
              "press +leftshift +rightshift -leftshift a -rightshift = \"A\"\n");
    while (n < 3 && keytable_next(&t, &c) == 1) {
        n++;
        CHECK(c.kind == KEYTABLE_SIGNAL && c.sig == 2, "SIGINT is Linux's 2");
        CHECK(c.modes == (KEYTABLE_TTY | KEYTABLE_ISIG), "the tty modes");
        last_ok = events_are(&c, both, 6);
    }
    CHECK(n == 3 && last_ok, "a press line with one class is 3 cases, the last on both sides");
    CHECK(keytable_next(&t, &c) == 1 && events_are(&c, fixed, 6) && c.modes == 0 && c.nbytes == 1 &&
              c.bytes[0] == 'A',
          "a press line of named keys is one case");
    CHECK(keytable_next(&t, &c) == 0, "the table ends after its lines");
    keytable_close(&t);
}

/** Each malformed line is refused, with its line number. */
static void check_refusals(void)
{
    static const char *const bad[] = {
        "nosuchkey \"a\" \"a\" \"a\" \"a\" \"a\" \"a\" \"a\" \"a\"\n",
        "a \"a\" \"A\"\n",
        "a \"1\" \"2\" \"3\" \"4\" \"5\" \"6\" \"7\" \"8\" \"9\"\n",
        "press a = SIGINT\n",
        "press a \"a\"\n",
        "press a =\n",
        "press = \"a\"\n",
        "press a = \"\\q\"\n",
        "press a = \"\\x4\"\n",
        "press a = \"open\n",
        "press a = \"a\"b\n",
        "press a = \"a\" \"b\"\n",
        "press +shiftx a = \"a\"\n",
        "press a = bogus\n",
        "modes bogus\n",
        "modes isig\n",
        "modes\n",
        "\"a\"\n",
    };
    struct keytable t;
    struct keytable_case c;
    char name[32], what[400];
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fix_format(name, sizeof(name), "bad%u.txt", (unsigned)i);
        fix_format(what, sizeof(what), "refused: %s", bad[i]);
        what[strcspn(what, "\n")] = '\0';
        open_text(&t, name, bad[i]);
        CHECK(keytable_next(&t, &c) == -1 && strstr(t.error, ":1: ") != NULL, what);
        keytable_close(&t);
    }
}

int main(void)
{
    check_table();
    check_key_line();
    check_press_line();
    check_refusals();
    if (failures) {
        fprintf(stderr, "keytable_hosttest: %d failures\n", failures);
        return 1;
    }
    fix_passed();
    printf("keytable_hosttest: ok\n");
    return 0;
}
