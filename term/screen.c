/*
 * screen.c: every call into libtsm but drawing's: the screen, the parser and
 * the replies it writes to the child. Task 16 completes it.
 */

#include <errno.h>
#include <stdlib.h>

#include "term.h"

/**
 * libtsm's write callback: the parser's replies to the child, such as the
 * answer to a device attributes request, go to the pty.
 */
static void screen_write(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
    struct term_screen *s = data;

    (void)vte;
    if (s->pty)
        term_pty_write(s->pty, u8, len);
}

/** libtsm's bell callback: the bell is shown at the next frame. */
static void screen_bell(struct tsm_vte *vte, void *data)
{
    struct term_screen *s = data;

    (void)vte;
    s->bell = true;
}

int term_screen_init(struct term_screen *s, struct term_pty *pty, unsigned cols, unsigned rows)
{
    int r;

    s->screen = NULL;
    s->vte = NULL;
    s->pty = pty;
    s->bell = false;
    s->clipboard = NULL;
    s->clipboard_len = 0;

    r = tsm_screen_new(&s->screen, NULL, NULL);
    if (r < 0)
        return r;
    r = tsm_screen_resize(s->screen, cols, rows);
    if (r >= 0)
        r = tsm_vte_new(&s->vte, s->screen, screen_write, s, NULL, NULL);
    if (r < 0) {
        tsm_screen_unref(s->screen);
        s->screen = NULL;
        return r;
    }
    tsm_vte_set_bell_cb(s->vte, screen_bell, s);
    return 0;
}

void term_screen_input(struct term_screen *s, const char *buf, size_t len)
{
    tsm_vte_input(s->vte, buf, len);
}

int term_screen_resize(struct term_screen *s, unsigned cols, unsigned rows)
{
    return tsm_screen_resize(s->screen, cols, rows);
}

bool term_screen_held(struct term_screen *s)
{
    return tsm_vte_get_sync_hold(s->vte);
}

void term_screen_release(struct term_screen *s)
{
    tsm_vte_end_sync_hold(s->vte);
}

void term_screen_focus(struct term_screen *s, bool focused)
{
    tsm_vte_handle_focus(s->vte, focused);
}

void term_screen_free(struct term_screen *s)
{
    tsm_vte_unref(s->vte);
    tsm_screen_unref(s->screen);
    free(s->clipboard);
    s->vte = NULL;
    s->screen = NULL;
    s->clipboard = NULL;
    s->clipboard_len = 0;
}
