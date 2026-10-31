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

#define TERM_CONF_PATH      "/etc/term.conf"
#define TERM_FONT_PATH      "/usr/share/term/unifont.font"
#define TERM_CONSOLE_PATH   "/dev/console"
#define TERM_SCROLLBACK     1000 /* lines of history kept unless the file says */
#define TERM_SCROLLBACK_MAX 100000

/*
 * Bytes of the child's output parsed before the loop looks at the keys
 * again, so that typing stays prompt while a program floods the screen.
 */
#define TERM_BURST 65536

/*
 * While the child holds the screen with mode 2026 and a frame waits, the
 * loop wakes at least this often, so that drawing can end a hold that has
 * lasted past its bound.
 */
#define TERM_HOLD_POLL_MS 20

/* What the command line asked for. */
struct term_options {
    bool login;        /* -l: run /bin/login, not the user's shell */
    bool host;         /* -H: run on a host pty with no screen, for tests */
    const char *conf;  /* -c FILE, or TERM_CONF_PATH */
    char *const *argv; /* the program to run and its arguments, or NULL */
};

/* The settings, from TERM_CONF_PATH or -c's file. */
struct term_conf {
    char font[256];      /* the packed font's path */
    unsigned scale;      /* the font's scale, 1 or 2 */
    unsigned scrollback; /* lines of history, 0 to TERM_SCROLLBACK_MAX */
};

/** Set every setting to its default. */
void term_conf_defaults(struct term_conf *c);

/**
 * Read the settings in `path` over those in `c`.
 *
 * Each line is `NAME = VALUE`, blank, or a comment that begins with `#`.
 * A line with an unknown name or a bad value is reported on stderr with its
 * number and skipped, and the setting keeps its value. A file that does not
 * exist is no error. Returns 0, or a negative errno value when the file
 * exists and cannot be read.
 */
int term_conf_read(struct term_conf *c, const char *path);

/* What term_signals_take() reports. */
#define TERM_SIG_CHILD 1u /* SIGCHLD: the child may have exited */
#define TERM_SIG_QUIT  2u /* SIGTERM, SIGHUP, SIGINT or SIGQUIT: leave */

/**
 * Catch SIGCHLD and the signals that end term, each written as a byte to a
 * pipe whose read end the loop polls, and ignore SIGPIPE.
 *
 * Returns the pipe's read end, nonblocking and close-on-exec, or a negative
 * errno value. term_pty_open()'s child restores the default dispositions.
 */
int term_signals_open(void);

/** Empty the pipe and return the TERM_SIG_* bits of what it held. */
unsigned term_signals_take(void);

/** Restore the default dispositions and close the pipe. */
void term_signals_close(void);

/**
 * The exit status a shell reports for wait status `status`: the child's
 * exit code, or 128 and the number of the signal that ended it.
 */
int term_status(int status);

/* ---- The pty (pty.c) ---- */

/* Bytes the child's input queue holds at most; term_pty_write() drops more. */
#define TERM_PTY_QUEUE_MAX (1024 * 1024)

/*
 * The child's terminal. The master is read for the child's output and
 * written with its input, through a queue, since a write to a master whose
 * slave is not read would block the loop.
 */
struct term_pty {
    int master;        /* the master's descriptor, nonblocking, or -1 */
    pid_t child;       /* the child, or -1 once it has been reaped */
    bool slave_open;   /* the child has opened its slave; read only then */
    char *queue;       /* bytes waiting to be written to the master */
    size_t queued;     /* bytes in the queue */
    size_t queue_size; /* bytes allocated for the queue */
};

/**
 * Open a pty of `cols` by `rows` cells and start `argv` on its slave, as the
 * leader of a new session with the slave as its controlling terminal, with
 * TERM=xterm-256color and COLORTERM=truecolor in its environment.
 *
 * `argv` is the program and its arguments, found on PATH; NULL runs
 * /bin/login when `login` is set, or else the shell SHELL names, /bin/sh
 * when it is unset. The master is close-on-exec and nonblocking.
 *
 * A master reads end of file while no slave is open, so the call returns
 * only once the child has opened its slave and executed the program; it
 * returns the program's exec error, negated, when that failed, having
 * reaped the child. On any error `pty` holds no descriptor and no child.
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
 * Returns the count read; 0 at the end of the child's output, when every
 * descriptor of the slave has closed, which Linux reports as EIO; -EAGAIN
 * when nothing is waiting; or another negative errno value.
 */
ssize_t term_pty_read(struct term_pty *pty, char *buf, size_t len);

/**
 * Queue `len` bytes for the child's input and write what the master takes
 * without blocking.
 *
 * Returns 0, -ENOBUFS when the queue would pass TERM_PTY_QUEUE_MAX and the
 * bytes are dropped, -EBADF when no master is open, or another negative
 * errno value when the master refuses them.
 */
int term_pty_write(struct term_pty *pty, const char *buf, size_t len);

/**
 * Write what the queue holds, without blocking.
 *
 * Returns 0 when the queue is empty afterwards, -EAGAIN when bytes remain,
 * or another negative errno value.
 */
int term_pty_flush(struct term_pty *pty);

/**
 * Reap the child if it has exited, without waiting.
 *
 * Returns 1 and stores its wait status in `*status` if it was reaped, 0 if
 * it still runs or there is none, or a negative errno value.
 */
int term_pty_reap(struct term_pty *pty, int *status);

/**
 * Close the master and free the queue.
 *
 * Closing the master sends SIGHUP to the slave's foreground process group;
 * the child is not otherwise signalled or reaped.
 */
void term_pty_close(struct term_pty *pty);

/* ---- The screen (screen.c) ---- */

/* Bytes the clipboard holds at most; OSC 52 sends more in base64. */
#define TERM_CLIPBOARD_MAX (1024 * 1024)

/*
 * libtsm's screen and parser, and what they report. Every call into libtsm
 * but drawing's tsm_screen_draw() is made from screen.c.
 */
struct term_screen {
    struct tsm_screen *screen;
    struct tsm_vte *vte;
    struct term_pty *pty; /* where the parser's replies are written */
    bool bell;            /* the bell rang; the loop shows it and clears this */
    char *clipboard;      /* the clipboard's text, from a selection or OSC 52 */
    size_t clipboard_len; /* its bytes; the text is also NUL-terminated */
};

/**
 * Make a screen of `cols` by `rows` cells whose replies go to `pty`.
 *
 * `pty` may be NULL, in which case replies are dropped.
 */
int term_screen_init(struct term_screen *s, struct term_pty *pty, unsigned cols, unsigned rows);

/** Keep at most `lines` lines of history above the screen. */
void term_screen_scrollback(struct term_screen *s, unsigned lines);

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

/**
 * Replace the clipboard with `len` bytes of `text`, which need not be
 * NUL-terminated. Returns 0, or -ENOMEM with the clipboard unchanged.
 */
int term_screen_set_clipboard(struct term_screen *s, const char *text, size_t len);

/* Code points a cell keeps in a snapshot: its character and the marks on it. */
#define TERM_CELL_CHARS 8

/* One cell of a snapshot. */
struct term_cell {
    uint32_t ch[TERM_CELL_CHARS]; /* the character and its combining marks */
    unsigned len;                 /* code points in `ch`; 0 for a blank cell */
    unsigned width;               /* columns: 1, 2, or 0 for a wide one's right half */
    struct tsm_screen_attr attr;  /* colors resolved to RGB in fr..bb */
};

/* The screen's cells as they would be drawn, for term -H. */
struct term_snapshot {
    unsigned cols;
    unsigned rows;
    unsigned cursor_x;
    unsigned cursor_y;
    bool cursor_visible;
    struct term_cell *cells; /* `rows` rows of `cols` cells, top row first */
};

/**
 * Copy the screen's cells into `snap`, which owns them afterwards.
 *
 * The cursor's cell is copied as the program left it, not inverted as
 * libtsm draws the cursor. The copy is made with tsm_screen_draw(), which
 * resets libtsm's record of what changed, so a screen that is drawn on the
 * framebuffer is never copied. Returns 0 or -ENOMEM.
 */
int term_screen_snapshot(struct term_screen *s, struct term_snapshot *snap);

/** Free a snapshot's cells. */
void term_snapshot_free(struct term_snapshot *snap);

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
    tsm_age_t age;          /* the screen's age at the last frame; 0 draws all */
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
 * Run the child on a host pty of 80 by 24 cells with no screen, for tests,
 * and answer the commands read from standard input on standard output, a
 * line each, as term/Readme.md lists them.
 *
 * At the end of standard input the master is closed, which hangs the child
 * up, and the child is reaped. Returns the exit status term should exit
 * with: the child's, as term_status() gives it, or 1 when it could not be
 * started.
 */
int term_host_run(const struct term_options *o, const struct term_conf *conf);

#endif /* TERM_H */
