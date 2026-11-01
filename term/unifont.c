/*
 * unifont.c: find a glyph in the packed Unifont that unifont.h describes.
 */
#include "unifont.h"

/** The 32-bit little-endian number at `p`. */
static unsigned long get32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 |
           (unsigned long)p[3] << 24;
}

/** The 16-bit little-endian number at `p`. */
static unsigned long get16(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8;
}

/**
 * Check run `i` of a font whose header is already checked.
 *
 * `next` is the lowest code point the run may begin at, and is set past its
 * last one. Returns the run's count of glyphs, or 0 when the run breaks a
 * rule of unifont.h: a count or width out of range, a code point out of
 * order or above UNIFONT_LAST, or glyphs that lie outside the file or over
 * its header and runs.
 */
static unsigned long check_run(const struct unifont *font, unsigned long i, unsigned long *next)
{
    const unsigned char *r = font->data + UNIFONT_HEADER + i * UNIFONT_RUN;
    unsigned long first = get32(r), offset = get32(r + 4);
    unsigned long count = get16(r + 8), bytes;

    if (count == 0 || (r[10] != 8 && r[10] != 16) || r[11] != 0)
        return 0;
    if (first < *next || first > UNIFONT_LAST || count - 1 > UNIFONT_LAST - first)
        return 0;
    bytes = count * (r[10] / 8UL) * UNIFONT_HEIGHT;
    if (offset < UNIFONT_HEADER + font->runs * UNIFONT_RUN || offset > font->size ||
        bytes > font->size - offset)
        return 0;
    *next = first + count;
    return count;
}

/**
 * Check the `size` bytes at `data` as a packed font and fill `font`.
 *
 * Every rule of unifont.h is checked here, so that unifont_glyph can trust
 * the file. The bytes are the caller's and must stay in place while `font`
 * is used. Returns 0, or -1 when the bytes are not such a font, with `font`
 * then undefined.
 */
int unifont_open(struct unifont *font, const unsigned char *data, unsigned long size)
{
    unsigned long i, n, next = 0, total = 0;
    const char *magic = UNIFONT_MAGIC;

    if (size < UNIFONT_HEADER)
        return -1;
    for (i = 0; i < 8; i++)
        if (data[i] != (unsigned char)magic[i])
            return -1;
    font->data = data;
    font->size = size;
    font->runs = get32(data + 8);
    font->glyphs = get32(data + 12);
    if (font->runs > (size - UNIFONT_HEADER) / UNIFONT_RUN)
        return -1;
    for (i = 0; i < font->runs; i++) {
        n = check_run(font, i, &next);
        if (n == 0)
            return -1;
        total += n;
    }
    return total == font->glyphs ? 0 : -1;
}

/**
 * Find the glyph of code point `code` in an opened font.
 *
 * Returns its first row, UNIFONT_HEIGHT rows of *width / 8 bytes each, with
 * *width set to 8 or 16; or a null pointer, with *width untouched, when the
 * font has no glyph for `code`. A binary search over the runs.
 */
const unsigned char *unifont_glyph(const struct unifont *font, unsigned long code, int *width)
{
    unsigned long lo = 0, hi = font->runs, mid, first;
    const unsigned char *r;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        r = font->data + UNIFONT_HEADER + mid * UNIFONT_RUN;
        first = get32(r);
        if (code < first) {
            hi = mid;
        } else if (code - first >= get16(r + 8)) {
            lo = mid + 1;
        } else {
            *width = r[10];
            return font->data + get32(r + 4) + (code - first) * (r[10] / 8UL) * UNIFONT_HEIGHT;
        }
    }
    return 0;
}
