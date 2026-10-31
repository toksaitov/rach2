/*
 * screen.c: every call into libtsm but drawing's: the screen, the parser,
 * the replies it writes to the child, and the bell, the clipboard and the
 * hold of mode 2026 that it reports.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

/** libtsm's bell callback: the loop shows the bell and clears the flag. */
static void screen_bell(struct tsm_vte *vte, void *data)
{
    struct term_screen *s = data;

    (void)vte;
    s->bell = true;
}

/** The value of base64 digit `c`, or -1 when it is none. */
static int base64_digit(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/**
 * Decode `len` bytes of base64 at `in` into `out`, which has room for
 * len / 4 * 3 bytes. Padding is optional; anything else that is not a digit
 * makes the text invalid.
 *
 * Returns the bytes decoded, or -1 for invalid text.
 */
static long base64_decode(const char *in, size_t len, char *out)
{
    unsigned long acc = 0;
    unsigned bits = 0;
    long n = 0;
    size_t i;
    int d;

    while (len > 0 && in[len - 1] == '=')
        len--;
    for (i = 0; i < len; i++) {
        d = base64_digit(in[i]);
        if (d < 0)
            return -1;
        acc = (acc << 6 | (unsigned long)d) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (char)(acc >> bits & 0xFF);
        }
    }
    return n;
}

/**
 * libtsm's clipboard callback, for OSC 52: the program's text, in base64,
 * becomes the clipboard's. A query, `?`, is not answered, so that no program
 * reads what another put there; text that is not base64 or would pass
 * TERM_CLIPBOARD_MAX is ignored.
 */
static void screen_clipboard(struct tsm_vte *vte, const char *targets, const char *payload,
                             size_t len, void *data)
{
    struct term_screen *s = data;
    char *text;
    long n;

    (void)vte;
    (void)targets;
    if (len == 1 && payload[0] == '?')
        return;
    if (len / 4 * 3 + 3 > TERM_CLIPBOARD_MAX)
        return;
    text = malloc(len / 4 * 3 + 3);
    if (!text)
        return;
    n = base64_decode(payload, len, text);
    if (n >= 0)
        term_screen_set_clipboard(s, text, (size_t)n);
    free(text);
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
    tsm_vte_set_clipboard_cb(s->vte, screen_clipboard, s);
    return 0;
}

void term_screen_scrollback(struct term_screen *s, unsigned lines)
{
    tsm_screen_set_max_sb(s->screen, lines);
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

int term_screen_set_clipboard(struct term_screen *s, const char *text, size_t len)
{
    char *copy = malloc(len + 1);

    if (!copy)
        return -ENOMEM;
    memcpy(copy, text, len);
    copy[len] = '\0';
    free(s->clipboard);
    s->clipboard = copy;
    s->clipboard_len = len;
    return 0;
}

/** tsm_screen_draw()'s callback for a snapshot: copy one cell. */
static int snapshot_cell(struct tsm_screen *con, uint64_t id, const uint32_t *ch, size_t len,
                         unsigned int width, unsigned int posx, unsigned int posy,
                         const struct tsm_screen_attr *attr, tsm_age_t age, void *data)
{
    struct term_snapshot *snap = data;
    struct term_cell *c;

    (void)con;
    (void)id;
    (void)age;
    if (posx >= snap->cols || posy >= snap->rows)
        return 0;
    c = &snap->cells[(size_t)posy * snap->cols + posx];
    if (len > TERM_CELL_CHARS)
        len = TERM_CELL_CHARS;
    memcpy(c->ch, ch, len * sizeof(ch[0]));
    c->len = (unsigned)len;
    c->width = width;
    c->attr = *attr;
    return 0;
}

int term_screen_snapshot(struct term_screen *s, struct term_snapshot *snap)
{
    unsigned flags = tsm_screen_get_flags(s->screen);

    snap->cols = tsm_screen_get_width(s->screen);
    snap->rows = tsm_screen_get_height(s->screen);
    snap->cursor_x = tsm_screen_get_cursor_x(s->screen);
    snap->cursor_y = tsm_screen_get_cursor_y(s->screen);
    snap->cursor_visible = !(flags & TSM_SCREEN_HIDE_CURSOR);
    snap->cells = calloc((size_t)snap->cols * snap->rows + 1, sizeof(snap->cells[0]));
    if (!snap->cells)
        return -ENOMEM;
    /* libtsm draws the cursor by inverting its cell; hide it for the copy. */
    if (snap->cursor_visible)
        tsm_screen_set_flags(s->screen, TSM_SCREEN_HIDE_CURSOR);
    tsm_screen_draw(s->screen, snapshot_cell, snap);
    if (snap->cursor_visible)
        tsm_screen_reset_flags(s->screen, TSM_SCREEN_HIDE_CURSOR);
    return 0;
}

void term_snapshot_free(struct term_snapshot *snap)
{
    free(snap->cells);
    snap->cells = NULL;
}

void term_screen_free(struct term_screen *s)
{
    if (s->vte)
        tsm_vte_unref(s->vte);
    if (s->screen)
        tsm_screen_unref(s->screen);
    free(s->clipboard);
    s->vte = NULL;
    s->screen = NULL;
    s->clipboard = NULL;
    s->clipboard_len = 0;
}
