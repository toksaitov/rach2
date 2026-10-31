/*
 * draw.c: libtsm's cells drawn into a back buffer and the rows that changed
 * written to the framebuffer. Task 17 writes it; until then each call fails
 * with ENOSYS.
 */

#include <errno.h>

#include "term.h"

int term_draw_open(struct term_draw *d, struct term_font *font)
{
    d->fd = -1;
    d->width = 0;
    d->height = 0;
    d->pitch = 0;
    d->bpp = 0;
    d->back = NULL;
    d->font = font;
    d->age = 0;
    return -ENOSYS;
}

void term_draw_cells(const struct term_draw *d, unsigned *cols, unsigned *rows)
{
    *cols = 0;
    *rows = 0;
    if (d->font && d->font->cell_width && d->font->cell_height) {
        *cols = d->width / d->font->cell_width;
        *rows = d->height / d->font->cell_height;
    }
}

int term_draw_frame(struct term_draw *d, struct term_screen *s)
{
    (void)d;
    (void)s;
    return -ENOSYS;
}

void term_draw_bell(struct term_draw *d)
{
    (void)d;
}

void term_draw_close(struct term_draw *d)
{
    (void)d;
}
