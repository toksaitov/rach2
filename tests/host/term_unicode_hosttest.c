/* Unicode cells and recorded agent output through term's real screen layer.
 * host-deps: term/screen.c term/pty.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-screen.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-render.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-selection.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-unicode.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte-charsets.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/shared/shl-htable.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.c
 * host-inputs: tests/fixtures/agents/claude-xterm-256color.typescript
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/tsm
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/shared
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external/wcwidth -I rach2/include
 * host-cflags: -include vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.h
 * host-cflags: -std=gnu99 -D_GNU_SOURCE -Dwcwidth=tsm_wcwidth
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../term/term.h"

static int failures;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "term-unicode: line %d: %s\n", __LINE__, #c); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

struct vector {
    const char *name;
    uint32_t text[8];
    unsigned width[8]; /* Per-code-point widths, before combining. */
};
static const struct vector vectors[] = {
    {"decomposed accent", {0x65, 0x301, 0}, {1, 0}},
    {"Cyrillic", {0x41A, 0x44B, 0x440, 0x433, 0x44B, 0x437, 0}, {1, 1, 1, 1, 1, 1}},
    {"CJK from the recording", {0x4F60, 0x597D, 0}, {2, 2}},
    {"variation selector", {0x2764, 0xFE0F, 0}, {1, 0}},
    {"emoji modifier", {0x1F44D, 0x1F3FD, 0}, {2, 2}},
    {"flag", {0x1F1F0, 0x1F1EC, 0}, {1, 1}},
    {"ZWJ sequence", {0x1F469, 0x200D, 0x1F4BB, 0}, {2, 0, 2}},
    {"rocket from the recording", {0x1F680, 0}, {2}},
};

/** Append one scalar as UTF-8, returning its byte count, from 1 through 4. */
static size_t utf8(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 63));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 63));
        out[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 63));
    out[2] = (char)(0x80 | (c >> 6 & 63));
    out[3] = (char)(0x80 | (c & 63));
    return 4;
}

/** Open an 80 by 25 screen with no pty, so parser replies have no receiver. */
static void open_screen(struct term_screen *s)
{
    if (term_screen_init(s, NULL, 80, 25) < 0) {
        fprintf(stderr, "term-unicode: cannot allocate screen\n");
        exit(1);
    }
}

/** Obtain an owned snapshot, ending the test on an allocation failure. */
static void snapshot(struct term_screen *s, struct term_snapshot *out)
{
    if (term_screen_snapshot(s, out) < 0) {
        term_screen_free(s);
        fprintf(stderr, "term-unicode: cannot allocate snapshot\n");
        exit(1);
    }
}

/** Check every cell occupied by a vector and the sentinel after its text. */
static void check_vector(const struct vector *v, size_t split)
{
    char bytes[64];
    size_t len = 0;
    unsigned x = 0, base = 0, marks = 0;
    struct term_screen s;
    struct term_snapshot snap;
    for (unsigned i = 0; v->text[i]; i++)
        len += utf8(bytes + len, v->text[i]);
    bytes[len++] = '|';
    open_screen(&s);
    term_screen_input(&s, bytes, split);
    term_screen_input(&s, bytes + split, len - split);
    snapshot(&s, &snap);
    for (unsigned i = 0; v->text[i]; i++) {
        if (v->width[i]) {
            const struct term_cell *c = &snap.cells[x];
            base = x;
            marks = 1;
            CHECK(c->width == v->width[i]);
            CHECK(c->len >= 1 && c->ch[0] == v->text[i]);
            if (v->width[i] == 2)
                CHECK(snap.cells[x + 1].width == 0 && snap.cells[x + 1].len == 0);
            x += v->width[i];
        } else {
            CHECK(snap.cells[base].len == marks + 1);
            CHECK(snap.cells[base].ch[marks] == v->text[i]);
            marks++;
        }
    }
    CHECK(snap.cells[x].len == 1 && snap.cells[x].ch[0] == '|');
    CHECK(snap.cursor_x == x + 1 && snap.cursor_y == 0);
    term_snapshot_free(&snap);
    term_screen_free(&s);
}

/** Compare text, widths, attributes and cursor after input fed two ways. */
static void equal(const struct term_snapshot *a, const struct term_snapshot *b)
{
    CHECK(a->cols == b->cols && a->rows == b->rows);
    CHECK(a->cursor_x == b->cursor_x && a->cursor_y == b->cursor_y);
    CHECK(a->cursor_visible == b->cursor_visible);
    if (a->cols != b->cols || a->rows != b->rows)
        return;
    for (size_t i = 0; i < (size_t)a->cols * a->rows; i++) {
        const struct term_cell *x = &a->cells[i], *y = &b->cells[i];
        CHECK(x->width == y->width && x->len == y->len);
        CHECK(!memcmp(x->ch, y->ch, sizeof(x->ch)));
        CHECK(x->attr.fr == y->attr.fr && x->attr.fg == y->attr.fg && x->attr.fb == y->attr.fb);
        CHECK(x->attr.br == y->attr.br && x->attr.bg == y->attr.bg && x->attr.bb == y->attr.bb);
        CHECK(x->attr.bold == y->attr.bold && x->attr.italic == y->attr.italic &&
              x->attr.underline == y->attr.underline && x->attr.inverse == y->attr.inverse);
    }
}

/** Replay the recording body, leaving script's header and trailer out. */
static void recording(void)
{
    static const char path[] = "tests/fixtures/agents/claude-xterm-256color.typescript";
    FILE *f = fopen(path, "rb");
    char *data, *body, *end;
    long length;
    struct term_screen whole, fragmented;
    struct term_snapshot a, b;
    unsigned seen = 0;
    if (!f || fseek(f, 0, SEEK_END) || (length = ftell(f)) <= 0 || length > 1024 * 1024 ||
        fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "term-unicode: cannot read recording: %s\n", path);
        if (f)
            fclose(f);
        failures++;
        return;
    }
    data = calloc((size_t)length + 1, 1);
    if (!data || fread(data, 1, (size_t)length, f) != (size_t)length) {
        fclose(f);
        free(data);
        failures++;
        return;
    }
    fclose(f);
    body = strchr(data, '\n');
    end = strstr(data, "\nScript done on ");
    if (!body || !end || end <= body || memchr(data, 0, (size_t)length)) {
        fprintf(stderr, "term-unicode: malformed recording\n");
        free(data);
        failures++;
        return;
    }
    body++;
    open_screen(&whole);
    open_screen(&fragmented);
    term_screen_input(&whole, body, (size_t)(end - body));
    for (char *p = body; p < end; p++) {
        term_screen_input(&fragmented, p, 1);
        if (((unsigned char)*p & 0xC0) != 0x80)
            continue;
        snapshot(&fragmented, &b);
        for (size_t i = 0; i < (size_t)b.cols * b.rows; i++) {
            const struct term_cell *c = &b.cells[i];
            if (!c->len)
                continue;
            if (c->ch[0] == 0x2705 || c->ch[0] == 0x1F680 || c->ch[0] == 0x4F60 ||
                c->ch[0] == 0x597D) {
                unsigned bit = c->ch[0] == 0x2705    ? 1
                               : c->ch[0] == 0x1F680 ? 2
                               : c->ch[0] == 0x4F60  ? 4
                                                     : 8;
                seen |= bit;
                CHECK(c->width == 2);
                CHECK(i % b.cols + 1 < b.cols);
                if (i % b.cols + 1 < b.cols)
                    CHECK(b.cells[i + 1].width == 0);
            }
        }
        term_snapshot_free(&b);
    }
    CHECK(seen == 15);
    snapshot(&whole, &a);
    snapshot(&fragmented, &b);
    equal(&a, &b);
    term_snapshot_free(&a);
    term_snapshot_free(&b);
    term_screen_free(&whole);
    term_screen_free(&fragmented);
    free(data);
}

/** Check all UTF-8 split points, then the agent's full screen updates. */
int main(void)
{
    for (size_t i = 0; i < sizeof(vectors) / sizeof(*vectors); i++) {
        char bytes[64];
        size_t len = 1;
        for (unsigned j = 0; vectors[i].text[j]; j++)
            len += utf8(bytes, vectors[i].text[j]);
        fprintf(stderr, "term-unicode: %s\n", vectors[i].name);
        for (size_t split = 0; split <= len; split++)
            check_vector(&vectors[i], split);
    }
    recording();
    fprintf(stderr, "term-unicode: result: %s\n", failures ? "failed" : "passed");
    return failures != 0;
}
