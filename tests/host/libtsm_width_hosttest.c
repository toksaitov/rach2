/* Wide glyph wrapping and combining symbols through libtsm's screen and VTE.
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-screen.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-render.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-selection.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-unicode.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/tsm/tsm-vte-charsets.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/src/shared/shl-htable.c
 * host-deps: vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.c
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/tsm
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/src/shared
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external
 * host-cflags: -I vendor/libtsm/libtsm-4.8.0/external/wcwidth
 * host-cflags: -include vendor/libtsm/libtsm-4.8.0/external/wcwidth/wcwidth.h
 * host-cflags: -std=gnu99 -D_GNU_SOURCE -Dwcwidth=tsm_wcwidth
 * host-cflags: -include tests/host/compat/libtsm_width_alloc.h
 */
#undef malloc
#undef calloc
#undef realloc
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"

static int failed;
/* Zero disables faults; otherwise fail this allocation and then disarm. */
static unsigned int fail_alloc;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "libtsm-width: line %d: %s\n", __LINE__, #c); \
            failed++;                                                     \
        }                                                                 \
    } while (0)

struct drawn_cell {
    uint32_t ch[10];
    size_t len;
    unsigned int width;
    struct tsm_screen_attr attr;
    tsm_age_t age;
    uint64_t id;
};

struct drawing {
    struct drawn_cell cells[4][6];
};

/** Allocate normally except for the selected call from the library. */
void *width_malloc(size_t size)
{
    if (fail_alloc && !--fail_alloc)
        return NULL;
    return malloc(size);
}

/** Allocate zeroed memory with the same fault counter as malloc. */
void *width_calloc(size_t count, size_t size)
{
    if (fail_alloc && !--fail_alloc)
        return NULL;
    return calloc(count, size);
}

/** Grow an allocation without changing the old block on injected failure. */
void *width_realloc(void *ptr, size_t size)
{
    if (fail_alloc && !--fail_alloc)
        return NULL;
    return realloc(ptr, size);
}

/** Copy the draw callback's borrowed text and attributes into a frame. */
static int draw_cell(struct tsm_screen *screen, uint64_t id, const uint32_t *ch, size_t len,
                     unsigned int width, unsigned int x, unsigned int y,
                     const struct tsm_screen_attr *attr, tsm_age_t age, void *data)
{
    struct drawing *draw = data;
    struct drawn_cell *cell;
    (void)screen;
    CHECK(x < 6 && y < 4 && len <= 10);
    if (x >= 6 || y >= 4 || len > 10)
        return -1;
    cell = &draw->cells[y][x];
    if (len)
        memcpy(cell->ch, ch, len * sizeof(*ch));
    cell->len = len;
    cell->width = width;
    cell->attr = *attr;
    cell->age = age;
    cell->id = id;
    return 0;
}

/** Capture the public draw interface, including its damage ages. */
static void capture(struct tsm_screen *screen, struct drawing *draw)
{
    memset(draw, 0, sizeof(*draw));
    tsm_screen_draw(screen, draw_cell, draw);
}

/** Compare one cell against independently specified code points and width. */
static void cell_is(struct drawing *draw, unsigned int x, unsigned int y, unsigned int width,
                    const uint32_t *ch, size_t len)
{
    struct drawn_cell *cell = &draw->cells[y][x];
    CHECK(cell->width == width);
    CHECK(cell->len == len);
    if (len && cell->len == len)
        CHECK(!memcmp(cell->ch, ch, len * sizeof(*ch)));
}

#define CELL(d, x, y, w, ...)                                                \
    do {                                                                     \
        const uint32_t expected[] = {__VA_ARGS__};                           \
        cell_is(d, x, y, w, expected, sizeof(expected) / sizeof(*expected)); \
    } while (0)

/** A small screen with autowrap and no cursor inversion; the caller owns it. */
static struct tsm_screen *new_screen(unsigned int width, unsigned int height)
{
    struct tsm_screen *screen = NULL;
    int rc = tsm_screen_new(&screen, NULL, NULL);
    CHECK(!rc);
    if (rc)
        exit(1);
    rc = tsm_screen_resize(screen, width, height);
    CHECK(!rc);
    if (rc) {
        tsm_screen_unref(screen);
        exit(1);
    }
    tsm_screen_set_flags(screen, TSM_SCREEN_AUTO_WRAP | TSM_SCREEN_HIDE_CURSOR);
    return screen;
}

/** Check early wrap, exact fit, pending wrap and both clipping policies. */
static void wide_glyphs(void)
{
    const uint32_t glyphs[] = {0x4e2d, 0x1f680, 0x1f916};
    struct tsm_screen_attr attr = {0};
    struct drawing draw;
    struct tsm_screen *screen;
    size_t i;
    for (i = 0; i < sizeof(glyphs) / sizeof(*glyphs); i++) {
        CHECK(tsm_ucs4_get_width(glyphs[i]) == 2);
        screen = new_screen(6, 3);
        tsm_screen_move_to(screen, 5, 0);
        tsm_screen_write(screen, glyphs[i], &attr);
        CHECK(tsm_screen_get_cursor_x(screen) == 2);
        CHECK(tsm_screen_get_cursor_y(screen) == 1);
        tsm_screen_write(screen, 'x', &attr);
        capture(screen, &draw);
        cell_is(&draw, 5, 0, 1, NULL, 0);
        CELL(&draw, 0, 1, 2, glyphs[i]);
        cell_is(&draw, 1, 1, 0, NULL, 0);
        CELL(&draw, 2, 1, 1, 'x');
        tsm_screen_move_to(screen, 4, 0);
        tsm_screen_write(screen, glyphs[i], &attr);
        CHECK(tsm_screen_get_cursor_x(screen) == 6);
        CHECK(tsm_screen_get_cursor_y(screen) == 0);
        tsm_screen_write(screen, 'y', &attr);
        capture(screen, &draw);
        CELL(&draw, 4, 0, 2, glyphs[i]);
        CELL(&draw, 0, 1, 1, 'y');
        tsm_screen_reset_flags(screen, TSM_SCREEN_AUTO_WRAP);
        tsm_screen_move_to(screen, 5, 2);
        tsm_screen_write(screen, glyphs[i], &attr);
        CHECK(tsm_screen_get_cursor_y(screen) == 2);
        capture(screen, &draw);
        CELL(&draw, 5, 2, 2, glyphs[i]);
        tsm_screen_unref(screen);
    }
    screen = new_screen(1, 2);
    tsm_screen_write(screen, 0x4e2d, &attr);
    CHECK(tsm_screen_get_cursor_y(screen) == 0);
    tsm_screen_write(screen, 'z', &attr);
    capture(screen, &draw);
    CELL(&draw, 0, 0, 2, 0x4e2d);
    CELL(&draw, 0, 1, 1, 'z');
    tsm_screen_unref(screen);
}

/** Require early wrap to scroll once at the bottom, also inside margins. */
static void wide_scroll(void)
{
    struct tsm_screen *screen = new_screen(6, 3);
    struct tsm_screen_attr attr = {0};
    struct drawing draw;
    unsigned int y;
    tsm_screen_set_max_sb(screen, 4);
    tsm_screen_write(screen, 'a', &attr);
    tsm_screen_move_to(screen, 5, 2);
    tsm_screen_write(screen, 0x1f680, &attr);
    CHECK(tsm_screen_get_cursor_y(screen) == 2);
    CHECK(tsm_screen_get_cursor_x(screen) == 2);
    CHECK(tsm_screen_sb_get_line_count(screen) == 1);
    capture(screen, &draw);
    CELL(&draw, 0, 2, 2, 0x1f680);
    tsm_screen_sb_up(screen, 1);
    capture(screen, &draw);
    CELL(&draw, 0, 0, 1, 'a');
    tsm_screen_unref(screen);

    screen = new_screen(6, 4);
    for (y = 0; y < 4; y++) {
        tsm_screen_move_to(screen, 0, y);
        tsm_screen_write(screen, 'a' + y, &attr);
    }
    CHECK(!tsm_screen_set_margins(screen, 2, 3));
    tsm_screen_move_to(screen, 5, 2);
    tsm_screen_write(screen, 0x4e2d, &attr);
    capture(screen, &draw);
    CELL(&draw, 0, 0, 1, 'a');
    CELL(&draw, 0, 1, 1, 'c');
    CELL(&draw, 0, 2, 2, 0x4e2d);
    CELL(&draw, 0, 3, 1, 'd');
    tsm_screen_unref(screen);
}

/** Preserve base attributes and repaint age across several decomposed marks. */
static void combining(void)
{
    struct tsm_screen *screen = new_screen(6, 3);
    struct tsm_screen_attr base = {0}, mark = {0};
    struct drawing before, after;
    unsigned int i;
    base.bold = 1;
    base.fr = 201;
    mark.underline = 1;
    mark.fr = 17;
    tsm_screen_write(screen, 'e', &base);
    capture(screen, &before);
    tsm_screen_write(screen, 0x301, &mark);
    capture(screen, &after);
    CELL(&after, 0, 0, 1, 'e', 0x301);
    CHECK(tsm_screen_get_cursor_x(screen) == 1);
    CHECK(after.cells[0][0].attr.bold && !after.cells[0][0].attr.underline);
    CHECK(after.cells[0][0].attr.fr == 201);
    CHECK(after.cells[0][0].age > before.cells[0][0].age);
    CHECK(after.cells[0][0].id != before.cells[0][0].id);
    tsm_screen_write(screen, 0x327, &mark);
    tsm_screen_write(screen, 0, &mark);
    tsm_screen_write(screen, 7, &mark);
    tsm_screen_write(screen, 0x7f, &mark);
    capture(screen, &after);
    CELL(&after, 0, 0, 1, 'e', 0x301, 0x327);
    tsm_screen_write(screen, '!', &base);
    capture(screen, &after);
    CELL(&after, 1, 0, 1, '!');

    tsm_screen_move_to(screen, 4, 1);
    tsm_screen_write(screen, 0x4e2d, &base);
    tsm_screen_write(screen, 0x301, &mark);
    CHECK(tsm_screen_get_cursor_x(screen) == 6);
    CHECK(tsm_screen_get_cursor_y(screen) == 1);
    capture(screen, &after);
    CELL(&after, 4, 1, 2, 0x4e2d, 0x301);
    cell_is(&after, 5, 1, 0, NULL, 0);
    tsm_screen_write(screen, 'x', &base);
    capture(screen, &after);
    CELL(&after, 0, 2, 1, 'x');

    tsm_screen_move_to(screen, 0, 0);
    tsm_screen_write(screen, 0x301, &mark);
    tsm_screen_move_to(screen, 3, 2);
    tsm_screen_write(screen, 0x301, &mark);
    capture(screen, &after);
    cell_is(&after, 2, 2, 1, NULL, 0);
    CELL(&after, 0, 0, 1, 'e', 0x301, 0x327);
    tsm_screen_move_to(screen, 5, 0);
    tsm_screen_write(screen, 'a', &base);
    for (i = 0; i < 15; i++)
        tsm_screen_write(screen, 0x301, &mark);
    capture(screen, &after);
    CHECK(after.cells[0][5].len == 10);
    CHECK(after.cells[0][5].ch[0] == 'a');
    for (i = 1; i < 10; i++)
        CHECK(after.cells[0][5].ch[i] == 0x301);
    CHECK(tsm_screen_get_cursor_x(screen) == 6);
    tsm_screen_unref(screen);
}

/** Discard terminal replies; the UTF-8 input rows produce no query response. */
static void vte_write(struct tsm_vte *vte, const char *text, size_t len, void *data)
{
    (void)vte;
    (void)text;
    (void)len;
    (void)data;
}

/** UTF-8 across writes, SGR, early wrap and the alternate screen kept apart. */
static void utf8_input(void)
{
    const char input[] = "abcde\033[1m\344\270\255\033[0m\314\201!";
    const char alternate[] = "\033[?1049h\033[H\303\266\314\201";
    struct tsm_screen *screen = new_screen(6, 3);
    struct tsm_vte *vte = NULL;
    struct drawing draw;
    size_t i;
    int rc = tsm_vte_new(&vte, screen, vte_write, NULL, NULL, NULL);
    CHECK(!rc);
    if (rc) {
        tsm_screen_unref(screen);
        return;
    }
    for (i = 0; i < sizeof(input) - 1; i++)
        tsm_vte_input(vte, &input[i], 1);
    capture(screen, &draw);
    CELL(&draw, 4, 0, 1, 'e');
    cell_is(&draw, 5, 0, 1, NULL, 0);
    CELL(&draw, 0, 1, 2, 0x4e2d, 0x301);
    CELL(&draw, 2, 1, 1, '!');
    CHECK(draw.cells[1][0].attr.bold);
    CHECK(tsm_screen_get_cursor_x(screen) == 3);
    CHECK(tsm_screen_get_cursor_y(screen) == 1);
    tsm_vte_input(vte, alternate, sizeof(alternate) - 1);
    capture(screen, &draw);
    CELL(&draw, 0, 0, 1, 0xf6, 0x301);
    tsm_vte_input(vte, "\033[?1049l", 8);
    capture(screen, &draw);
    CELL(&draw, 0, 1, 2, 0x4e2d, 0x301);
    tsm_vte_unref(vte);
    tsm_screen_unref(screen);
}

/** Fail each allocation in a symbol's creation, then retry on one table. */
static void allocation_failures(void)
{
    struct tsm_screen_attr attr = {0};
    struct drawing draw;
    unsigned int warm, fault, i;
    for (warm = 0; warm <= 3; warm += 3) {
        for (fault = 1; fault <= 3; fault++) {
            struct tsm_screen *screen = new_screen(6, 2);
            for (i = 0; i < warm; i++) {
                tsm_screen_move_to(screen, i, 0);
                tsm_screen_write(screen, 'a' + i, &attr);
                tsm_screen_write(screen, 0x301, &attr);
            }
            tsm_screen_move_to(screen, 0, 1);
            tsm_screen_write(screen, 'z', &attr);
            fail_alloc = fault;
            tsm_screen_write(screen, 0x327, &attr);
            CHECK(!fail_alloc);
            fail_alloc = 0;
            capture(screen, &draw);
            CELL(&draw, 0, 1, 1, 'z');
            CHECK(tsm_screen_get_cursor_x(screen) == 1);
            tsm_screen_write(screen, 0x327, &attr);
            capture(screen, &draw);
            CELL(&draw, 0, 1, 1, 'z', 0x327);
            for (i = 0; i < warm; i++)
                CELL(&draw, i, 0, 1, 'a' + i, 0x301);
            tsm_screen_unref(screen);
        }
    }
}

/** The terminal's public behaviour, with no files and no child processes. */
int main(void)
{
    wide_glyphs();
    wide_scroll();
    combining();
    utf8_input();
    allocation_failures();
    if (failed)
        return 1;
    puts("libtsm-width: result: passed");
    return 0;
}
