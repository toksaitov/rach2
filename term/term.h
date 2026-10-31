/*
 * term.h: the parts of term, the terminal that runs on the whole framebuffer,
 * and the calls between them, a section for each part and its file.
 */

#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <libtsm.h>

/*
 * Every call that can fail returns 0 or a negative errno value, as libtsm's
 * do, unless its comment says otherwise.
 */

/* ---- The program (main.c) ---- */

/* What the command line asked for. */
struct term_options {
    bool login;        /* -l: run /bin/login, not the user's shell */
    bool host;         /* -H: run on a host pty with no screen, for tests */
    const char *conf;  /* the configuration file, /etc/term.conf */
    char *const *argv; /* the program to run and its arguments, or NULL */
};

/* ---- The pty (pty.c) ---- */

/*
 * The child's terminal. The master is read for the child's output and
 * written with its input, through a queue, since a write to a master whose
 * slave is not read would block the loop.
 */
struct term_pty {
    int master;        /* the master's descriptor, or -1 */
    pid_t child;       /* the child, or -1 once it has been reaped */
    bool slave_open;   /* the child has opened its slave; read only then */
    char *queue;       /* bytes waiting to be written to the master */
    size_t queued;     /* bytes in the queue */
    size_t queue_size; /* bytes allocated for the queue */
};

/**
 * Open a pty of `cols` by `rows` cells and start `argv` on its slave, as the
 * leader of a new session with the slave as its controlling terminal.
 *
 * The master is opened close-on-exec. `argv` is the program and its
 * arguments; NULL runs the user's shell, or /bin/login when `login` is set.
 */
int term_pty_open(struct term_pty *pty, unsigned cols, unsigned rows, char *const *argv,
                  bool login);

/**
 * Tell the pty its new size, which sends SIGWINCH to the child's foreground
 * process group.
 */
int term_pty_resize(struct term_pty *pty, unsigned cols, unsigned rows);

/**
 * Read what the child wrote, at most `len` bytes.
 *
 * Returns the count read, 0 at the end of the child's output, or a negative
 * errno value; -EAGAIN when nothing is waiting.
 */
ssize_t term_pty_read(struct term_pty *pty, char *buf, size_t len);

/**
 * Queue `len` bytes for the child's input and write what the master takes
 * without blocking.
 */
int term_pty_write(struct term_pty *pty, const char *buf, size_t len);

/**
 * Write what the queue holds, without blocking.
 *
 * Returns 0 when the queue is empty afterwards, -EAGAIN when bytes remain.
 */
int term_pty_flush(struct term_pty *pty);

/**
 * Reap the child if it has exited, without waiting.
 *
 * Returns 1 and stores its wait status in `*status` if it was reaped, 0 if
 * it still runs, or a negative errno value.
 */
int term_pty_reap(struct term_pty *pty, int *status);

/** Close the master and free the queue; the child is not signalled. */
void term_pty_close(struct term_pty *pty);

/* ---- The screen (screen.c) ---- */

/*
 * libtsm's screen and parser, and what they report. Every call into libtsm
 * but drawing's tsm_screen_draw() is made from screen.c.
 */
struct term_screen {
    struct tsm_screen *screen;
    struct tsm_vte *vte;
    struct term_pty *pty; /* where the parser's replies are written */
    bool bell;            /* the bell rang since drawing last looked */
    char *clipboard;      /* the clipboard's text, from a selection or OSC 52 */
    size_t clipboard_len;
};

/**
 * Make a screen of `cols` by `rows` cells whose replies go to `pty`.
 *
 * `pty` may be NULL, in which case replies are dropped.
 */
int term_screen_init(struct term_screen *s, struct term_pty *pty, unsigned cols, unsigned rows);

/** Parse `len` bytes of the child's output onto the screen. */
void term_screen_input(struct term_screen *s, const char *buf, size_t len);

/** Change the screen's size to `cols` by `rows` cells. */
int term_screen_resize(struct term_screen *s, unsigned cols, unsigned rows);

/**
 * Whether the child holds the screen's updates with mode 2026.
 *
 * Drawing keeps the last frame while this is true, and calls
 * term_screen_release() when the hold has lasted past its bound.
 */
bool term_screen_held(struct term_screen *s);

/** End the child's hold of mode 2026. */
void term_screen_release(struct term_screen *s);

/** Tell the child, under mode 1004, that the screen gained or lost the focus. */
void term_screen_focus(struct term_screen *s, bool focused);

/** Free the screen, the parser and the clipboard. */
void term_screen_free(struct term_screen *s);

/* ---- The font (font.c) ---- */

/* The packed Unifont at the scale in use. */
struct term_font {
    const unsigned char *data; /* the packed font, or NULL before loading */
    size_t size;               /* bytes at `data` */
    unsigned scale;            /* 1 or 2 */
    unsigned cell_width;       /* a cell's width in pixels, at the scale */
    unsigned cell_height;      /* a cell's height in pixels, at the scale */
};

/** Load the packed font from `path` at `scale`, 1 or 2. */
int term_font_load(struct term_font *f, const char *path, unsigned scale);

/**
 * The glyph of `ucs4` at scale 1, one bit a pixel, rows of whole bytes.
 *
 * Stores its width in pixels, 8 or 16, in `*width`. Returns NULL when the
 * font has no glyph for the character.
 */
const unsigned char *term_font_glyph(const struct term_font *f, uint32_t ucs4, unsigned *width);

/** Change the scale to `scale`, 1 or 2, which changes the cell's size. */
int term_font_scale(struct term_font *f, unsigned scale);

/** Release the font. */
void term_font_free(struct term_font *f);

/* ---- Drawing (draw.c) ---- */

/*
 * The framebuffer and the back buffer in RAM that frames are drawn into
 * before the rows that changed are written to it.
 */
struct term_draw {
    int fd;                 /* /fb, or -1 */
    unsigned width;         /* the framebuffer's width in pixels */
    unsigned height;        /* its height in pixels */
    unsigned pitch;         /* bytes from one row to the next */
    unsigned bpp;           /* bits a pixel */
    unsigned char *back;    /* the back buffer, `pitch` * `height` bytes */
    struct term_font *font; /* the font cells are drawn with */
    tsm_age_t age;          /* the screen's age at the last frame */
};

/**
 * Open the framebuffer, at its native mode, and the back buffer, and draw
 * with `font`.
 */
int term_draw_open(struct term_draw *d, struct term_font *font);

/** The screen's size in cells with the font in use. */
void term_draw_cells(const struct term_draw *d, unsigned *cols, unsigned *rows);

/**
 * Draw what changed on `s` since the last frame, unless the child holds
 * the screen.
 */
int term_draw_frame(struct term_draw *d, struct term_screen *s);

/** Show the bell. */
void term_draw_bell(struct term_draw *d);

/** Close the framebuffer and free the back buffer. */
void term_draw_close(struct term_draw *d);

/* ---- Selection (select.c) ---- */

/* The selection made from the keyboard, in cells of the screen. */
struct term_select {
    bool active;     /* the selection mode is on and takes the keys */
    bool marked;     /* the start has been marked */
    unsigned x, y;   /* the cursor of the selection mode */
    unsigned mark_x; /* the start's column */
    unsigned mark_y; /* the start's row */
};

/**
 * Give a key to the selection mode.
 *
 * Returns true if the mode took the key; false if it is off, and the key
 * goes to the child.
 */
bool term_select_key(struct term_select *sel, struct term_screen *s, uint32_t keysym,
                     unsigned mods);

/** Paste the clipboard into the child, bracketed when mode 2004 is set. */
void term_select_paste(struct term_select *sel, struct term_screen *s);

/** Leave the selection mode and forget the selection. */
void term_select_clear(struct term_select *sel, struct term_screen *s);

/* ---- Keys (keys.c) ---- */

/* The console's keyboard, read in K_MEDIUMRAW through the console's keymap. */
struct term_keys {
    int fd;                 /* the console, or -1 */
    int saved_mode;         /* the keyboard's mode before term took it, or -1 */
    unsigned char down[32]; /* a bit for each keycode held down */
};

/** Open the console `path` and take its keyboard. */
int term_keys_open(struct term_keys *k, const char *path);

/**
 * Read the keys that are waiting and give each to the selection mode or to
 * the child.
 *
 * Returns 0 when nothing more is waiting, or a negative errno value.
 */
int term_keys_read(struct term_keys *k, struct term_screen *s, struct term_select *sel);

/** Forget which keys are down, as when the screen comes back to term. */
void term_keys_reset(struct term_keys *k);

/** Give the keyboard its mode back and close the console. */
void term_keys_close(struct term_keys *k);

/* ---- Host mode (host.c) ---- */

/**
 * Run the child on a host pty with no screen, for tests: standard input
 * goes to the child, and the screen's cells are dumped on request.
 *
 * Returns the exit status term should exit with.
 */
int term_host_run(const struct term_options *o);

#endif /* TERM_H */
