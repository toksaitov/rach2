/**
 * Host test of the packed Unifont: the converter tools/font/mkunifont.c and
 * term's reader, term/unifont.c.
 *
 * host-deps: term/unifont.c
 *
 * The converter is included whole, its main renamed. The text below is
 * fifteen lines of unifont_all-18.0.01.hex as GNU publishes it, one of them
 * ending in a carriage return and the last in no newline. Packed, it must
 * give the Latin A, the Cyrillic Zhe, the Armenian Ayb, the braille cell of
 * all six dots, the CJK U+4E2D and the plane 1 face U+1F600 their rows and
 * widths, typed out below from the same lines, and no glyph to the code
 * points between them. U+10FF and U+1100 are consecutive but 8 and 16 wide,
 * so they begin two runs; the fifteen glyphs make nine runs in all.
 * 70,000 consecutive glyphs make two runs, since a run holds at most 65,535.
 *
 * The converter refuses each kind of bad line, naming its number, and the
 * reader refuses a font with any field of its header or a run broken.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main mkunifont_main
#include "../../tools/font/mkunifont.c"
#undef main

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

static const char sample[] =
    "0040:000000001C224A565252524E201E0000\n"
    "0041:0000000018242442427E424242420000\n"
    "0042:000000007C4242427C424242427C0000\r\n"
    "0043:000000003C42424040404042423C0000\n"
    "0416:0000000049492A2A1C1C2A2A49490000\n"
    "0531:00000000444444444444444745380000\n"
    "10FF:0000000000000000001C2222221C0000\n"
    "1100:00000000000000000FF800080008000800080008000800080008000000000000\n"
    "2800:00000022000022000022000022000000\n"
    "2801:00000032300022000022000022000000\n"
    "28FF:00000036360036360036360036360000\n"
    "4E2D:01000100010001003FF8210821082108210821083FF821080100010001000100\n"
    "4E2E:00007FF00410041004107FD0041004100410FFD00412040A040A040604020400\n"
    "01F600:000003E00C181004200226324631400140014FF92AAA26B213E40C1803E00000\n"
    "01F601:000003E00C181004200226324949400140014FF92AAA26B213E40C1803E00000";

static const unsigned char latin_a[16] = {0x00, 0x00, 0x00, 0x00, 0x18, 0x24, 0x24, 0x42,
                                          0x42, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00};
static const unsigned char cyrillic_zhe[16] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x49, 0x2A, 0x2A,
                                               0x1C, 0x1C, 0x2A, 0x2A, 0x49, 0x49, 0x00, 0x00};
static const unsigned char armenian_ayb[16] = {0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x44, 0x44,
                                               0x44, 0x44, 0x44, 0x47, 0x45, 0x38, 0x00, 0x00};
static const unsigned char braille_123456[16] = {0x00, 0x00, 0x00, 0x36, 0x36, 0x00, 0x36, 0x36,
                                                 0x00, 0x36, 0x36, 0x00, 0x36, 0x36, 0x00, 0x00};
static const unsigned char cjk_middle[32] = {
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x3F, 0xF8, 0x21, 0x08, 0x21, 0x08, 0x21, 0x08,
    0x21, 0x08, 0x21, 0x08, 0x3F, 0xF8, 0x21, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00};
static const unsigned char face_grinning[32] = {
    0x00, 0x00, 0x03, 0xE0, 0x0C, 0x18, 0x10, 0x04, 0x20, 0x02, 0x26, 0x32, 0x46, 0x31, 0x40, 0x01,
    0x40, 0x01, 0x4F, 0xF9, 0x2A, 0xAA, 0x26, 0xB2, 0x13, 0xE4, 0x0C, 0x18, 0x03, 0xE0, 0x00, 0x00};

/** Pack the NUL-terminated .hex text `text`; *size is the font's length.
 * Returns the font, or a null pointer with the fault printed. */
static unsigned char *pack_text(const char *text, unsigned long *size)
{
    unsigned long line;
    const char *err = "";
    unsigned char *font;

    font = mkunifont(text, strlen(text), size, &line, &err);
    if (!font)
        fprintf(stderr, "  line %lu: %s\n", line, err);
    return font;
}

/** Count a failure, named `what`, unless `font` has the glyph `want` of
 * `width` pixels for `code`. */
static void expect_glyph(const struct unifont *font, unsigned long code, const unsigned char *want,
                         int width, const char *what)
{
    const unsigned char *rows;
    int got = 0;

    rows = unifont_glyph(font, code, &got);
    CHECK(rows != 0, what);
    if (!rows)
        return;
    CHECK(got == width, what);
    if (got == width)
        CHECK(memcmp(rows, want, (size_t)width * 2) == 0, what);
}

/** Count a failure unless `font` has no glyph for `code`, and leaves the
 * width it was given alone. */
static void expect_none(const struct unifont *font, unsigned long code)
{
    int width = 99;
    char what[64];

    sprintf(what, "no glyph for U+%04lX", code);
    CHECK(unifont_glyph(font, code, &width) == 0 && width == 99, what);
}

/** The glyphs of the sample, their runs, and the gaps between them. */
static void test_sample(void)
{
    struct unifont font;
    unsigned char *data;
    unsigned long size;

    data = pack_text(sample, &size);
    CHECK(data != 0, "the sample packs");
    if (!data)
        return;
    CHECK(size == UNIFONT_HEADER + 9 * UNIFONT_RUN + 11 * 16 + 4 * 32,
          "the sample's font has 9 runs, 11 narrow glyphs and 4 wide ones");
    CHECK(unifont_open(&font, data, size) == 0, "the sample's font opens");
    CHECK(font.runs == 9 && font.glyphs == 15, "9 runs of 15 glyphs");
    expect_glyph(&font, 0x41, latin_a, 8, "U+0041 LATIN CAPITAL LETTER A");
    expect_glyph(&font, 0x416, cyrillic_zhe, 8, "U+0416 CYRILLIC ZHE");
    expect_glyph(&font, 0x531, armenian_ayb, 8, "U+0531 ARMENIAN AYB");
    expect_glyph(&font, 0x28FF, braille_123456, 8, "U+28FF BRAILLE 1-8");
    expect_glyph(&font, 0x4E2D, cjk_middle, 16, "U+4E2D CJK");
    expect_glyph(&font, 0x1F600, face_grinning, 16, "U+1F600 FACE");
    expect_none(&font, 0);
    expect_none(&font, 0x3F);
    expect_none(&font, 0x44);
    expect_none(&font, 0x415);
    expect_none(&font, 0x27FF);
    expect_none(&font, 0x2802);
    expect_none(&font, 0x1F602);
    expect_none(&font, 0x110000);
    free(data);
}

/** 70,000 consecutive glyphs, each with its index in its first rows, make
 * two runs, and a glyph on each side of the break reads back. */
static void test_long_run(void)
{
    enum { N = 70000 };
    struct unifont font;
    unsigned char *data, want[16];
    unsigned long size, i;
    char *text = malloc((size_t)N * 40 + 1), *p = text;

    CHECK(text != 0, "memory for 70,000 lines");
    if (!text)
        return;
    for (i = 0; i < N; i++)
        p += sprintf(p, "%06lX:%06lX00000000000000000000000000\n", 0x20000 + i, i);
    data = pack_text(text, &size);
    free(text);
    CHECK(data != 0, "70,000 glyphs pack");
    if (!data)
        return;
    CHECK(unifont_open(&font, data, size) == 0 && font.runs == 2,
          "70,000 consecutive glyphs make two runs");
    memset(want, 0, sizeof want);
    for (i = 65534; i <= 65536; i++) {
        want[0] = (unsigned char)(i >> 16);
        want[1] = (unsigned char)(i >> 8);
        want[2] = (unsigned char)i;
        expect_glyph(&font, 0x20000 + i, want, 8, "a glyph by a run's end");
    }
    expect_none(&font, 0x20000 + N);
    free(data);
}

/** The converter refuses `text`, naming line `line`. */
static void refuse(const char *text, unsigned long line, const char *what)
{
    unsigned long size, got = 99;
    const char *err = "";
    unsigned char *font;

    font = mkunifont(text, strlen(text), &size, &got, &err);
    CHECK(font == 0 && got == line, what);
    free(font);
}

static void test_refusals(void)
{
    refuse("", 0, "an empty text");
    refuse("0041:0000000018242442427E424242420000\n\n", 2, "an empty line");
    refuse("0042:000000007C4242427C424242427C0000\n"
           "0041:0000000018242442427E424242420000\n",
           2, "a code point below the one before");
    refuse("0041:0000000018242442427E424242420000\n"
           "0041:0000000018242442427E424242420000\n",
           2, "a code point twice");
    refuse("041:0000000018242442427E424242420000\n", 1, "three digits");
    refuse("0000041:0000000018242442427E424242420000\n", 1, "seven digits");
    refuse("110000:0000000018242442427E424242420000\n", 1, "U+110000");
    refuse("00G1:0000000018242442427E424242420000\n", 1, "a bad code");
    refuse("0041 0000000018242442427E424242420000\n", 1, "no colon");
    refuse("0041:0000000018242442427E4242424200\n", 1, "30 digits");
    refuse("0041:0000000018242442427E42424242000000\n", 1, "34 digits");
    refuse("0041:000000001824244242ZE424242420000\n", 1, "a bad digit");
}

/** Count a failure unless the sample's font, with byte `at` set to `v`
 * and cut to `size` bytes, fails to open. */
static void corrupt(const unsigned char *good, unsigned long size, unsigned long at,
                    unsigned char v, const char *what)
{
    struct unifont font;
    unsigned char *bad = malloc(size);

    CHECK(bad != 0, "memory for a copy");
    if (!bad)
        return;
    memcpy(bad, good, size);
    if (at < size)
        bad[at] = v;
    CHECK(unifont_open(&font, bad, size) < 0, what);
    free(bad);
}

static void test_reader(void)
{
    unsigned char *data;
    unsigned long size, run = UNIFONT_HEADER + UNIFONT_RUN;

    data = pack_text(sample, &size);
    CHECK(data != 0, "the sample packs again");
    if (!data)
        return;
    corrupt(data, size - 1, size, 0, "a font cut short by a byte");
    corrupt(data, UNIFONT_HEADER - 1, size, 0, "a font shorter than a header");
    corrupt(data, size, 0, 'r', "a wrong magic");
    corrupt(data, size, 8, 200, "more runs than the file holds");
    corrupt(data, size, 12, 16, "a glyph count the runs do not sum to");
    corrupt(data, size, run + 1, 0, "a run below the one before it");
    corrupt(data, size, run + 6, 1, "a run past the end of the file");
    corrupt(data, size, run + 4, 0, "a run over the header");
    corrupt(data, size, run + 8, 0, "a run of no glyphs");
    corrupt(data, size, run + 10, 12, "a width of 12");
    corrupt(data, size, run + 11, 1, "a nonzero reserved byte");
    corrupt(data, size, run + 2, 0x11, "a run above U+10FFFF");
    free(data);
}

int main(void)
{
    test_sample();
    test_long_run();
    test_refusals();
    test_reader();
    if (fails) {
        fprintf(stderr, "unifont: %d failures\n", fails);
        return 1;
    }
    printf("unifont: all checks passed\n");
    return 0;
}
