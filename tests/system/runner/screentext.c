/*
 * screentext: the text a framebuffer console shows, read off a screen dump.
 *
 * Usage: screentext DUMP
 *        screentext --colors DUMP
 *
 * DUMP is QEMU's screen dump, a binary PPM (P6, 255 levels). The console's
 * grid is found as the kernel's fbcon.c lays it out: the 8 by 16 font at the
 * largest whole scale at which the screen still holds 80 columns and 24
 * rows, as many cells as fit at it, within 240 by 64, centred on the screen.
 * Each row of cells is printed as a line of text. A cell whose pixels are
 * all of one colour is a space; any other is the glyph of the kernel's own
 * font (rach2/sys/display/vga_font.c) whose set pixels are those of a colour
 * other than the cell's top-left one, compared with the cursor's two
 * underline rows left out when a cell has them all set, so that the cursor
 * on a blank cell is a space; a cell no glyph matches is printed as '?', and
 * the glyph of a code outside printable ASCII as '#'. Trailing spaces are
 * left off each line. With --colors it prints instead how many colours the
 * screen has, which tells a picture from the console's sixteen. Exits 0, or
 * 1 with a diagnostic for a usage error, a file that is no such dump or is
 * too small for the grid, or no memory to count the colours.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The console's fewest and most columns and rows, its font's cell, and the
 * two rows of the cell it draws the cursor in, as fbcon.c and textcon.h
 * have them. */
#define MIN_COLS     80
#define MIN_ROWS     24
#define MAX_COLS     240
#define MAX_ROWS     64
#define GLYPH_WIDTH  8
#define GLYPH_HEIGHT 16
#define CURSOR_FIRST 13
#define CURSOR_LAST  14

/* The font's record, as vga_font.h lays it out, in the host's types: the
 * kernel's vga_font.c is compiled and linked with this file. */
struct vga_glyph {
    unsigned char slot;
    unsigned char rows[GLYPH_HEIGHT];
};

extern const struct vga_glyph vga_font_all[256];

/* The dump's pixels, three bytes each in rows of `width`, and its size. */
static unsigned char *pixels;
static unsigned long width, height;

/** The colour of the pixel at (x, y), as one number. */
static unsigned long colour(unsigned long x, unsigned long y)
{
    const unsigned char *p = pixels + (y * width + x) * 3u;

    return (unsigned long)p[0] << 16 | (unsigned long)p[1] << 8 | p[2];
}

/**
 * Read the next number of a PPM header into `*out`, past white space and
 * `#` comments. Returns 1, or 0 when no digit comes next or the number grows
 * past ten million, so that a dimension the caller refuses cannot overflow
 * on the way.
 */
static int header_number(FILE *f, unsigned long *out)
{
    int c = fgetc(f);

    for (;;) {
        while (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            c = fgetc(f);
        }
        if (c != '#') {
            break;
        }
        while (c != '\n' && c != EOF) {
            c = fgetc(f);
        }
    }
    if (c < '0' || c > '9') {
        return 0;
    }
    *out = 0;
    while (c >= '0' && c <= '9') {
        if (*out > 1000000u) {
            return 0;
        }
        *out = *out * 10u + (unsigned long)(c - '0');
        c = fgetc(f);
    }
    return 1;
}

/**
 * The character the cell at (`col`, `row`) shows, read from its pixels at
 * `scale` times the font's size with the grid's corner at (`left`, `top`).
 *
 * A pixel is set when its colour differs from the cell's top-left pixel. A
 * cell with none set is a space; otherwise the cell's code is the first
 * whose glyph has the same rows, with the cursor's two rows left out of the
 * comparison when both are full. The result is the character for a
 * printable code, a space for code 0, whose glyph is blank, '#' for any
 * other code, and '?' when no glyph matches.
 */
static char cell_char(unsigned long left, unsigned long top, unsigned long scale, int col, int row)
{
    unsigned char bits[GLYPH_HEIGHT];
    unsigned long x0 = left + (unsigned long)col * GLYPH_WIDTH * scale;
    unsigned long y0 = top + (unsigned long)row * GLYPH_HEIGHT * scale;
    unsigned long bg = colour(x0, y0);
    int r, c, any = 0, code, cursor;

    for (r = 0; r < GLYPH_HEIGHT; r++) {
        bits[r] = 0;
        for (c = 0; c < GLYPH_WIDTH; c++) {
            if (colour(x0 + (unsigned long)c * scale, y0 + (unsigned long)r * scale) != bg) {
                bits[r] |= (unsigned char)(0x80u >> c);
                any = 1;
            }
        }
    }
    if (!any) {
        return ' ';
    }
    cursor = bits[CURSOR_FIRST] == 0xFF && bits[CURSOR_LAST] == 0xFF;
    for (code = 0; code < 256; code++) {
        const unsigned char *g = vga_font_all[code].rows;

        for (r = 0; r < GLYPH_HEIGHT; r++) {
            if ((r == CURSOR_FIRST || r == CURSOR_LAST) && cursor) {
                continue;
            }
            if (g[r] != bits[r]) {
                break;
            }
        }
        if (r == GLYPH_HEIGHT) {
            return code >= 0x20 && code < 0x7F ? (char)code : code == 0 ? ' ' : '#';
        }
    }
    return '?';
}

/** The number of colours among the screen's pixels, or 0 when there is no
 * memory for the map of the colours seen. */
static unsigned long colours(void)
{
    unsigned char *seen = calloc(1u << 21, 1);
    unsigned long i, n = 0;

    if (!seen) {
        return 0;
    }
    for (i = 0; i < width * height; i++) {
        unsigned long c =
            (unsigned long)pixels[3 * i] << 16 | pixels[3 * i + 1] << 8 | pixels[3 * i + 2];

        if (!(seen[c >> 3] & (1u << (c & 7)))) {
            seen[c >> 3] |= (unsigned char)(1u << (c & 7));
            n++;
        }
    }
    free(seen);
    return n;
}

/** Read the screen dump at `path` into `pixels`, `width` and `height`.
 * Returns 0, or 1 with a diagnostic for a file that cannot be opened, is no
 * binary PPM of 255 levels, or is short of its pixels. */
static int read_dump(const char *path)
{
    unsigned long maxval;
    FILE *f = fopen(path, "rb");

    if (!f) {
        fprintf(stderr, "screentext: %s: cannot open\n", path);
        return 1;
    }
    if (fgetc(f) != 'P' || fgetc(f) != '6' || !header_number(f, &width) ||
        !header_number(f, &height) || !header_number(f, &maxval) || maxval != 255 || width == 0 ||
        height == 0 || width > 16384 || height > 16384) {
        fprintf(stderr, "screentext: %s: not a binary PPM of 255 levels\n", path);
        fclose(f);
        return 1;
    }
    pixels = malloc(width * height * 3u);
    if (!pixels || fread(pixels, 3u, width * height, f) != width * height) {
        fprintf(stderr, "screentext: %s: short of its pixels\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/**
 * Read the dump, then print its colour count, or find the console's grid as
 * fbcon.c lays it out and print each row of cells as a line.
 */
int main(int argc, char **argv)
{
    unsigned long scale, left, top, cols, rows;
    char line[MAX_COLS + 1];
    int row, col, end, count = argc == 3 && strcmp(argv[1], "--colors") == 0;
    const char *path = argv[argc - 1];

    if (argc != 2 && !count) {
        fprintf(stderr, "usage: screentext [--colors] DUMP\n");
        return 1;
    }
    if (read_dump(path) != 0) {
        return 1;
    }
    if (count) {
        unsigned long n = colours();

        free(pixels);
        if (n == 0) {
            fprintf(stderr, "screentext: %s: no memory to count the colours\n", path);
            return 1;
        }
        printf("%lu\n", n);
        return 0;
    }
    if (width / GLYPH_WIDTH < MIN_COLS || height / GLYPH_HEIGHT < MIN_ROWS) {
        fprintf(stderr, "screentext: %s: %lux%lu is smaller than the console's grid\n", path, width,
                height);
        return 1;
    }
    for (scale = 1; width / (GLYPH_WIDTH * (scale + 1)) >= MIN_COLS &&
                    height / (GLYPH_HEIGHT * (scale + 1)) >= MIN_ROWS;
         scale++) {
    }
    cols = width / (GLYPH_WIDTH * scale);
    rows = height / (GLYPH_HEIGHT * scale);
    cols = cols > MAX_COLS ? MAX_COLS : cols;
    rows = rows > MAX_ROWS ? MAX_ROWS : rows;
    left = (width - cols * GLYPH_WIDTH * scale) / 2u;
    top = (height - rows * GLYPH_HEIGHT * scale) / 2u;
    for (row = 0; row < (int)rows; row++) {
        for (col = 0; col < (int)cols; col++) {
            line[col] = cell_char(left, top, scale, col, row);
        }
        for (end = (int)cols; end > 0 && line[end - 1] == ' '; end--) {
        }
        line[end] = '\0';
        printf("%s\n", line);
    }
    free(pixels);
    return 0;
}
