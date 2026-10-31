/*
 * mkunifont.c: pack GNU Unifont's .hex text into the font file term draws
 * with, in the format term/unifont.h describes.
 *
 * Usage: mkunifont HEX OUT
 *
 * HEX is the text of unifont_all-*.hex: one glyph a line, its code point in
 * four to six hexadecimal digits, a colon and its 16 rows in 32 hexadecimal
 * digits for a glyph 8 pixels wide or 64 for one 16 wide, the lines in
 * ascending order of code point. Every glyph is packed. Before it writes OUT
 * the program opens the packed font with term's own reader and requires
 * every glyph to read back as the text has it, so that a font the build
 * makes is one term can read. mk/unifont.mk runs it at build time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../term/unifont.h"

/** The glyphs read from the .hex text, in its order. */
struct glyphs {
    unsigned long *code;  /* the code point of each */
    unsigned char *width; /* 8 or 16 */
    unsigned char *rows;  /* 32 bytes each; a narrow glyph uses 16 */
    size_t n, cap;
};

/** The value of hexadecimal digit `c`, or -1. */
static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/**
 * Parse the line of `len` bytes at `s` into glyph `g->n`, which has room.
 *
 * Returns 0, or -1 with `err` set when the line is not a code point, a colon
 * and 32 or 64 hexadecimal digits, or its code point is above UNIFONT_LAST.
 */
static int parse_line(struct glyphs *g, const char *s, size_t len, const char **err)
{
    size_t i, digits, colon;
    unsigned long code = 0;
    unsigned char *rows = g->rows + g->n * 32;

    for (colon = 0; colon < len && s[colon] != ':'; colon++)
        ;
    if (colon < 4 || colon > 6 || colon == len) {
        *err = "not a code point of 4 to 6 digits and a colon";
        return -1;
    }
    for (i = 0; i < colon; i++) {
        if (hexval(s[i]) < 0) {
            *err = "a code point that is not hexadecimal";
            return -1;
        }
        code = code << 4 | (unsigned long)hexval(s[i]);
    }
    if (code > UNIFONT_LAST) {
        *err = "a code point above U+10FFFF";
        return -1;
    }
    digits = len - colon - 1;
    if (digits != 32 && digits != 64) {
        *err = "a glyph that is not 32 or 64 digits";
        return -1;
    }
    for (i = 0; i < digits; i += 2) {
        int hi = hexval(s[colon + 1 + i]), lo = hexval(s[colon + 2 + i]);

        if (hi < 0 || lo < 0) {
            *err = "a glyph that is not hexadecimal";
            return -1;
        }
        rows[i / 2] = (unsigned char)(hi << 4 | lo);
    }
    g->code[g->n] = code;
    g->width[g->n] = (unsigned char)(digits / 4);
    return 0;
}

/** Make room for one more glyph. Returns 0, or -1 when memory runs out. */
static int grow(struct glyphs *g)
{
    size_t cap = g->cap ? g->cap * 2 : 1024;
    void *code, *width, *rows;

    if (g->n < g->cap)
        return 0;
    code = realloc(g->code, cap * sizeof *g->code);
    if (code)
        g->code = code;
    width = realloc(g->width, cap);
    if (width)
        g->width = width;
    rows = realloc(g->rows, cap * 32);
    if (rows)
        g->rows = rows;
    if (!code || !width || !rows)
        return -1;
    g->cap = cap;
    return 0;
}

/**
 * Read the .hex text of `len` bytes at `text` into `g`, which starts empty.
 *
 * A line may end in a carriage return before its newline, and the last
 * line needs no newline; an empty line is refused. Returns 0, or -1 with
 * *line the number of the line at fault, counting from 1, and `err` set.
 * `g` holds what was read either way; free it with free_glyphs.
 */
static int read_hex(struct glyphs *g, const char *text, size_t len, unsigned long *line,
                    const char **err)
{
    size_t at = 0, end, n;

    *line = 0;
    while (at < len) {
        ++*line;
        for (end = at; end < len && text[end] != '\n'; end++)
            ;
        n = end - at;
        if (n > 0 && text[at + n - 1] == '\r')
            n--;
        if (grow(g) < 0) {
            *err = "out of memory";
            return -1;
        }
        if (parse_line(g, text + at, n, err) < 0)
            return -1;
        if (g->n > 0 && g->code[g->n] <= g->code[g->n - 1]) {
            *err = "a code point not above the line before";
            return -1;
        }
        g->n++;
        at = end + 1;
    }
    if (g->n == 0) {
        *err = "no glyphs";
        return -1;
    }
    return 0;
}

/** Free what read_hex allocated. */
static void free_glyphs(struct glyphs *g)
{
    free(g->code);
    free(g->width);
    free(g->rows);
}

/** Store `v` at `p` as a little-endian number of `n` bytes. */
static void put(unsigned char *p, unsigned long v, int n)
{
    int i;

    for (i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> 8 * i);
}

/** Whether glyph `i` of `g` begins a run: it does not follow glyph i - 1
 * in code point and width, or that run is full at `count` glyphs. */
static int starts_run(const struct glyphs *g, size_t i, unsigned long count)
{
    return i == 0 || g->code[i] != g->code[i - 1] + 1 || g->width[i] != g->width[i - 1] ||
           count == UNIFONT_RUN_GLYPHS;
}

/**
 * Pack the glyphs of `g` into a font as unifont.h describes.
 *
 * Returns the font, of *size bytes, which the caller frees, or a null
 * pointer when memory runs out.
 */
static unsigned char *pack(const struct glyphs *g, unsigned long *size)
{
    unsigned long runs = 0, count = 0, at, bytes = 0;
    unsigned char *font, *run = 0;
    size_t i;

    for (i = 0; i < g->n; i++) {
        count = starts_run(g, i, count) ? 1 : count + 1;
        runs += count == 1;
        bytes += g->width[i] * 2UL;
    }
    at = UNIFONT_HEADER + runs * UNIFONT_RUN;
    *size = at + bytes;
    font = calloc(1, *size);
    if (!font)
        return 0;
    memcpy(font, UNIFONT_MAGIC, 8);
    put(font + 8, runs, 4);
    put(font + 12, (unsigned long)g->n, 4);
    for (i = 0; i < g->n; i++) {
        if (run == 0 || starts_run(g, i, count)) {
            run = run ? run + UNIFONT_RUN : font + UNIFONT_HEADER;
            put(run, g->code[i], 4);
            put(run + 4, at, 4);
            run[10] = g->width[i];
            count = 0;
        }
        put(run + 8, ++count, 2);
        memcpy(font + at, g->rows + i * 32, g->width[i] * 2UL);
        at += g->width[i] * 2UL;
    }
    return font;
}

/**
 * Open a packed font with term's reader and require each glyph of `g` to
 * read back as `g` has it. Returns 0, or -1 with `err` set.
 */
static int verify(const struct glyphs *g, const unsigned char *data, unsigned long size,
                  const char **err)
{
    struct unifont font;
    const unsigned char *rows;
    int width;
    size_t i;

    if (unifont_open(&font, data, size) < 0) {
        *err = "the packed font does not open";
        return -1;
    }
    for (i = 0; i < g->n; i++) {
        rows = unifont_glyph(&font, g->code[i], &width);
        if (!rows || width != g->width[i] ||
            memcmp(rows, g->rows + i * 32, g->width[i] * 2UL) != 0) {
            *err = "a glyph does not read back from the packed font";
            return -1;
        }
    }
    return 0;
}

/**
 * Pack the .hex text of `len` bytes at `text` and check the result.
 *
 * Returns the font, of *size bytes, which the caller frees; or a null
 * pointer with `err` set, and *line the number of the line at fault or 0
 * when the fault is no one line's.
 */
static unsigned char *mkunifont(const char *text, size_t len, unsigned long *size,
                                unsigned long *line, const char **err)
{
    struct glyphs g;
    unsigned char *font = 0;

    memset(&g, 0, sizeof g);
    if (read_hex(&g, text, len, line, err) == 0) {
        *line = 0;
        font = pack(&g, size);
        if (!font)
            *err = "out of memory";
        else if (verify(&g, font, *size, err) < 0) {
            free(font);
            font = 0;
        }
    }
    free_glyphs(&g);
    return font;
}

/** Read the whole file `path`. Returns its bytes, which the caller frees,
 * with *len set, or a null pointer. */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = 0, *more;
    size_t cap = 0, n;

    *len = 0;
    if (!f)
        return 0;
    for (;;) {
        if (*len == cap) {
            cap = cap ? cap * 2 : 1 << 20;
            more = realloc(buf, cap);
            if (!more)
                break;
            buf = more;
        }
        n = fread(buf + *len, 1, cap - *len, f);
        *len += n;
        if (n == 0) {
            if (ferror(f))
                break;
            fclose(f);
            return buf;
        }
    }
    fclose(f);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    char *text;
    unsigned char *font;
    unsigned long size, line;
    size_t len;
    const char *err = "";
    FILE *out;

    if (argc != 3) {
        fprintf(stderr, "usage: mkunifont HEX OUT\n");
        return 2;
    }
    text = slurp(argv[1], &len);
    if (!text) {
        fprintf(stderr, "mkunifont: %s: cannot read\n", argv[1]);
        return 1;
    }
    font = mkunifont(text, len, &size, &line, &err);
    free(text);
    if (!font) {
        if (line)
            fprintf(stderr, "mkunifont: %s:%lu: %s\n", argv[1], line, err);
        else
            fprintf(stderr, "mkunifont: %s: %s\n", argv[1], err);
        return 1;
    }
    out = fopen(argv[2], "wb");
    if (out) {
        if (fwrite(font, 1, size, out) != size)
            size = 0;
        if (fclose(out) != 0)
            size = 0;
    }
    free(font);
    if (!out || size == 0) {
        fprintf(stderr, "mkunifont: %s: cannot write\n", argv[2]);
        return 1;
    }
    return 0;
}
