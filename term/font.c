/*
 * font.c: the packed Unifont that term draws with, at 1x or 2x.
 * Task 17 writes it; until then each call fails with ENOSYS.
 */

#include <errno.h>

#include "term.h"

int term_font_load(struct term_font *f, const char *path, unsigned scale)
{
    (void)path;
    f->data = NULL;
    f->size = 0;
    f->scale = scale;
    f->cell_width = 8 * scale;
    f->cell_height = 16 * scale;
    return -ENOSYS;
}

const unsigned char *term_font_glyph(const struct term_font *f, uint32_t ucs4, unsigned *width)
{
    (void)f;
    (void)ucs4;
    *width = 8;
    return NULL;
}

int term_font_scale(struct term_font *f, unsigned scale)
{
    (void)f;
    (void)scale;
    return -ENOSYS;
}

void term_font_free(struct term_font *f)
{
    f->data = NULL;
    f->size = 0;
}
