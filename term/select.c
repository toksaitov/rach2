/*
 * select.c: the selection made from the keyboard, the clipboard and paste.
 * Task 22 writes it; until then the selection mode is never on.
 */

#include "term.h"

bool term_select_key(struct term_select *sel, struct term_screen *s, uint32_t keysym, unsigned mods)
{
    (void)sel;
    (void)s;
    (void)keysym;
    (void)mods;
    return false;
}

void term_select_paste(struct term_select *sel, struct term_screen *s)
{
    (void)sel;
    (void)s;
}

void term_select_clear(struct term_select *sel, struct term_screen *s)
{
    (void)s;
    sel->active = false;
    sel->marked = false;
}
