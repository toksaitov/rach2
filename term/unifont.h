/*
 * unifont.h: the packed GNU Unifont that term draws with, the file
 * tools/font/mkunifont.c writes and unifont.c reads.
 */
#ifndef TERM_UNIFONT_H
#define TERM_UNIFONT_H

/*
 * The file holds every glyph of Unifont's .hex text, each 16 rows high and 8
 * or 16 pixels wide, grouped in runs of consecutive code points of one width.
 * Every number is unsigned and little-endian, so the file is the same on
 * every host and both architectures.
 *
 *     offset  size  field
 *          0     8  UNIFONT_MAGIC, "R2UFONT1", no terminator
 *          8     4  the number of runs
 *         12     4  the number of glyphs, the sum of the runs' counts
 *         16    12  the first run, then the others, by code point
 *
 * A run is 12 bytes:
 *
 *          0     4  its first code point
 *          4     4  the file offset of its first glyph
 *          8     2  its count of glyphs, 1 to UNIFONT_RUN_GLYPHS
 *         10     1  its width in pixels, 8 or 16
 *         11     1  zero
 *
 * The glyph of code point C in a run is at its offset plus (C - first) times
 * the glyph's size: 16 bytes for a width of 8, 32 for 16. Its rows go from
 * top to bottom, each of width / 8 bytes, left byte first; the high bit of a
 * byte is the leftmost pixel, and a set bit is the foreground, as in the
 * .hex text. Runs do not overlap and each begins above the last code point
 * of the run before it; a code point in no run has no glyph.
 */
#define UNIFONT_MAGIC "R2UFONT1"
#define UNIFONT_HEADER 16      /* bytes before the first run */
#define UNIFONT_RUN 12         /* bytes of a run */
#define UNIFONT_HEIGHT 16      /* rows of every glyph */
#define UNIFONT_RUN_GLYPHS 65535UL
#define UNIFONT_LAST 0x10FFFFUL /* the last code point Unicode has */

/** A packed font checked by unifont_open; the bytes stay the caller's. */
struct unifont {
    const unsigned char *data; /* the whole file */
    unsigned long size;        /* its length in bytes */
    unsigned long runs;        /* the number of runs */
    unsigned long glyphs;      /* the number of glyphs */
};

int unifont_open(struct unifont *font, const unsigned char *data,
                 unsigned long size);
const unsigned char *unifont_glyph(const struct unifont *font,
                                   unsigned long code, int *width);

#endif
