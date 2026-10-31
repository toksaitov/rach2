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
#include <rach2/fb.h>

#include "unifont.h"

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
 * is made from screen.c but drawing's, which reads the screen it draws:
 * tsm_screen_draw(), the cursor and the scrollback's position.
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
    unsigned char *data;  /* the packed font, read whole, or NULL */
    size_t size;          /* bytes at `data` */
    struct unifont index; /* the font as unifont_open() checked it */
    unsigned scale;       /* 1 or 2: each pixel of a glyph is scale by scale */
    unsigned cell_width;  /* a cell's width in pixels, 8 * scale */
    unsigned cell_height; /* a cell's height in pixels, 16 * scale */
};

/**
 * Read the packed font from `path` and check it, at `scale`, 1 or 2.
 *
 * Returns -EINVAL for a scale other than 1 or 2 and for a file that is not a
 * packed font, or the errno value of the open or read that failed.
 */
int term_font_load(struct term_font *f, const char *path, unsigned scale);

/**
 * The glyph of `ucs4` at scale 1, one bit a pixel, 16 rows of whole bytes.
 *
 * Stores its width in pixels, 8 or 16, in `*width`. Returns NULL, with
 * `*width` untouched, when the font has no glyph for the character.
 */
const unsigned char *term_font_glyph(const struct term_font *f, uint32_t ucs4, unsigned *width);

/** Change the scale to `scale`, 1 or 2, which changes the cell's size. */
int term_font_scale(struct term_font *f, unsigned scale);

/** Free the font's bytes. */
void term_font_free(struct term_font *f);

/**
 * Keep `scale` in the configuration file `conf`, as the line `scale = N`
 * that term_conf_read() reads: the file's `scale` line is replaced, or the
 * line added when it has none, and every other line is kept.
 *
 * The file is written as `conf` with ".new" appended and renamed over
 * `conf`, so that a failed write leaves the old file whole.
 */
int term_font_conf_save(const char *conf, unsigned scale);

/* ---- Drawing (draw.c) ---- */

/* How long mode 2026 may hold the screen before a frame is drawn anyway. */
#define TERM_DRAW_HOLD_MS 250
/* How long the visible bell shows the screen in inverse colors. */
#define TERM_DRAW_BELL_MS 100
/* Glyphs kept rendered, a power of two. */
#define TERM_DRAW_GLYPHS  1024

/* The code points of a glyph that the cache keeps; longer ones are not kept. */
#define TERM_GLYPH_POINTS 4

/*
 * A glyph as drawing renders it at scale 1: the cell's glyphs, its combining
 * marks among them, put over each other, made bold, slanted or underlined.
 * It is found by its code points and shape, not by libtsm's id of the cell,
 * since libtsm gives a combined character's id the bit it gives bold.
 */
struct term_glyph {
    uint32_t ch[TERM_GLYPH_POINTS]; /* the code points, the base first */
    unsigned char len;              /* their count; 0 for an empty cell */
    /* 1 bold, 2 italic, 4 underline, 8 two cells wide; 0x80 when in use */
    unsigned char shape;
    uint32_t rows[16]; /* the rows, top first; bit 31 is the leftmost pixel */
};

/*
 * The cell that tsm_screen_draw() reported before the one being drawn, kept
 * so that the second column of a wide character can draw the first again.
 */
struct term_draw_cell {
    bool valid;                  /* a cell of the row before this one */
    bool drawn;                  /* it was drawn in this frame */
    const uint32_t *ch;          /* its code points, valid during the frame */
    size_t len;                  /* their count; 0 for an empty cell */
    unsigned width;              /* its width in cells, 0, 1 or 2 */
    unsigned x, y;               /* its column and row */
    struct tsm_screen_attr attr; /* its attributes as libtsm gave them */
};

/*
 * The framebuffer, at its native mode, and the back buffer in RAM that
 * frames are drawn into before the bytes that changed are written to it.
 * The back buffer has the framebuffer's layout, so a byte's offset in it is
 * its offset for pwrite(2), and it holds what the screen shows.
 */
struct term_draw {
    int fd;                 /* /fb, or a file of the same layout; -1 */
    struct fb_screen info;  /* the framebuffer, as FB_GETINFO reports it */
    unsigned bytes;         /* bytes a pixel: 2, 3 or 4 */
    unsigned char *back;    /* the back buffer, info.pitch * info.height */
    struct term_font *font; /* the font cells are drawn with */
    unsigned cols, rows;    /* the screen in cells, at the font's scale */
    /* Per cell row, the first pixel column changed and the last plus one;
     * the two are equal when none changed. */
    unsigned *damage;
    struct term_glyph *glyphs; /* TERM_DRAW_GLYPHS rendered glyphs */
    /* The screen's age at the last frame; 0 draws every cell and the
     * margins, as after the screen comes back. */
    tsm_age_t age;
    bool full;         /* the next frame draws every cell */
    bool cursor_shown; /* the cursor as the last frame drew it */
    unsigned cursor_x, cursor_y;
    int cursor_style;      /* an enum tsm_screen_cursor_style */
    uint64_t bell_until;   /* when the visible bell ends, or 0 */
    uint64_t held_since;   /* when mode 2026's hold began, or 0 */
    uint64_t (*now)(void); /* milliseconds of a monotonic clock, never 0 */
    /* The frame being drawn. */
    bool frame_cursor;          /* the cursor shows in this frame */
    unsigned frame_x, frame_y;  /* where tsm_screen_draw() puts it */
    struct term_draw_cell prev; /* the cell drawn before the current one */
};

/**
 * Open /fb, enter its native mode and draw on it with `font`.
 *
 * Returns -ENODEV for a framebuffer whose format drawing cannot write, an
 * indexed one or a text console, or the errno value of the call that failed.
 */
int term_draw_open(struct term_draw *d, struct term_font *font);

/**
 * Draw with `font` on `fd`, a framebuffer that `info` describes, already in
 * its native mode, or a file of `info->pitch` times `info->height` bytes.
 *
 * `info` must be RGB at 15, 16, 24 or 32 bits a pixel, else -ENODEV. The
 * first frame draws every cell, and the margins past the last cell in the
 * default background. `d` closes `fd` at term_draw_close().
 */
int term_draw_init(struct term_draw *d, struct term_font *font, int fd,
                   const struct fb_screen *info);

/** The screen's size in cells with the font in use. */
void term_draw_cells(const struct term_draw *d, unsigned *cols, unsigned *rows);

/**
 * Draw what changed on `s` since the last frame, unless the child holds the
 * screen with mode 2026 for less than TERM_DRAW_HOLD_MS.
 *
 * Returns 0, or the errno value of the write that failed, after which the
 * next frame draws every cell.
 */
int term_draw_frame(struct term_draw *d, struct term_screen *s);

/**
 * The milliseconds until term_draw_frame() must be called again although
 * nothing changed, for the bell's end or the hold's bound; -1 when none.
 */
int term_draw_timeout(const struct term_draw *d);

/** Show the bell at the next frame. */
void term_draw_bell(struct term_draw *d);

/** Draw every cell at the next frame, as when the screen comes back. */
void term_draw_redraw(struct term_draw *d);

/**
 * Change the font's scale to `scale`, 1 or 2, and with it the screen's size
 * in cells, and keep the scale in `conf` unless it is NULL. The loop sees
 * the new size in term_draw_cells() and gives it to libtsm and the pty.
 *
 * A failure to keep the scale does not undo the change; its errno value is
 * returned.
 */
int term_draw_rescale(struct term_draw *d, unsigned scale, const char *conf);

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
