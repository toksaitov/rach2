/*
 * keys.c: the console's keyboard in K_MEDIUMRAW, turned into libtsm's key
 * events through the console's keymap. Task 22 writes it; until then each
 * call fails with ENOSYS.
 */

#include <errno.h>
#include <string.h>

#include "term.h"

int term_keys_open(struct term_keys *k, const char *path)
{
    (void)path;
    k->fd = -1;
    k->saved_mode = -1;
    memset(k->down, 0, sizeof(k->down));
    return -ENOSYS;
}

int term_keys_read(struct term_keys *k, struct term_screen *s, struct term_select *sel)
{
    (void)k;
    (void)s;
    (void)sel;
    return -ENOSYS;
}

void term_keys_reset(struct term_keys *k)
{
    memset(k->down, 0, sizeof(k->down));
}

void term_keys_close(struct term_keys *k)
{
    (void)k;
}
