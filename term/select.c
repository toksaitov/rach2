/*
 * select.c: the selection made from the keyboard, which copies the screen's
 * text into the clipboard that OSC 52 also sets, and paste from it.
 */

#include <stdlib.h>
#include <string.h>

#include "term.h"

/* The keysyms the selection mode reads, XKB's values. */
#define SYM_RETURN    0xff0du
#define SYM_ESCAPE    0xff1bu
#define SYM_HOME      0xff50u
#define SYM_LEFT      0xff51u
#define SYM_UP        0xff52u
#define SYM_RIGHT     0xff53u
#define SYM_DOWN      0xff54u
#define SYM_PAGE_UP   0xff55u
#define SYM_PAGE_DOWN 0xff56u
#define SYM_END       0xff57u
#define SYM_KP_ENTER  0xff8du

/** Show where the mode's cursor is: the one cell, or the mark to it. */
static void show(struct term_select *sel, struct term_screen *s)
{
    term_screen_select(s, sel->marked, sel->x, sel->y);
}

void term_select_start(struct term_select *sel, struct term_screen *s)
{
    unsigned cols;
    unsigned rows;

    term_screen_size(s, &cols, &rows);
    term_screen_cursor(s, &sel->x, &sel->y);
    if (sel->x >= cols)
        sel->x = cols ? cols - 1 : 0;
    if (sel->y >= rows)
        sel->y = rows ? rows - 1 : 0;
    sel->active = true;
    sel->marked = false;
    show(sel, s);
}

/**
 * Move the mode's cursor down by `dy` rows, up when negative; past the top or
 * the bottom row the scrollback moves instead, and the mark with its line.
 */
static void move_rows(struct term_select *sel, struct term_screen *s, int dy, unsigned rows)
{
    int y = (int)sel->y + dy;

    if (y < 0) {
        term_screen_scroll(s, -y);
        y = 0;
    } else if (y >= (int)rows) {
        term_screen_scroll(s, -(y - (int)rows + 1));
        y = (int)rows - 1;
    }
    sel->y = (unsigned)y;
}

bool term_select_key(struct term_select *sel, struct term_screen *s, uint32_t keysym, unsigned mods)
{
    unsigned cols;
    unsigned rows;

    (void)mods;
    if (!sel->active)
        return false;
    term_screen_size(s, &cols, &rows);
    if (cols == 0 || rows == 0)
        return true;
    switch (keysym) {
    case SYM_LEFT:
    case 'h':
        if (sel->x > 0)
            sel->x--;
        break;
    case SYM_RIGHT:
    case 'l':
        if (sel->x + 1 < cols)
            sel->x++;
        break;
    case SYM_UP:
    case 'k':
        move_rows(sel, s, -1, rows);
        break;
    case SYM_DOWN:
    case 'j':
        move_rows(sel, s, 1, rows);
        break;
    case SYM_PAGE_UP:
        move_rows(sel, s, -(int)rows, rows);
        break;
    case SYM_PAGE_DOWN:
        move_rows(sel, s, (int)rows, rows);
        break;
    case SYM_HOME:
    case '0':
        sel->x = 0;
        break;
    case SYM_END:
    case '$':
        sel->x = cols - 1;
        break;
    case ' ':
    case 'v':
        sel->marked = true;
        sel->mark_x = sel->x;
        sel->mark_y = sel->y;
        term_screen_select(s, false, sel->x, sel->y);
        return true;
    case SYM_RETURN:
    case SYM_KP_ENTER:
    case 'y':
        if (sel->marked)
            term_screen_select_copy(s);
        term_select_clear(sel, s);
        return true;
    case SYM_ESCAPE:
    case 'q':
        term_select_clear(sel, s);
        return true;
    default:
        return true;
    }
    show(sel, s);
    return true;
}

void term_select_paste(struct term_select *sel, struct term_screen *s)
{
    const char *in = s->clipboard;
    size_t len = s->clipboard_len;
    size_t i;
    size_t n = 0;
    char *text;

    (void)sel;
    if (!in || len == 0)
        return;
    text = malloc(len + 1);
    if (!text)
        return;
    for (i = 0; i < len; i++) {
        if (in[i] == '\033')
            continue;
        if (in[i] == '\n') {
            if (n == 0 || text[n - 1] != '\r')
                text[n++] = '\r';
            continue;
        }
        text[n++] = in[i];
    }
    text[n] = '\0';
    if (n > 0)
        term_screen_paste(s, text);
    memset(text, 0, n);
    free(text);
}

void term_select_clear(struct term_select *sel, struct term_screen *s)
{
    if (sel->active || sel->marked) {
        term_screen_select_reset(s);
        term_screen_scroll_reset(s);
    }
    sel->active = false;
    sel->marked = false;
}
