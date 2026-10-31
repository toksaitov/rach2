/*
 * keytable.h: the reader of keytable.txt, the bytes each key of a 105-key
 * keyboard sends as xterm sends them, expanded into cases of key events.
 */

#ifndef KEYTABLE_H
#define KEYTABLE_H

#include <stddef.h>
#include <stdio.h>

/* The table's path from the top of the tree, where host tests run. */
#define KEYTABLE_PATH "tests/host/keytable.txt"

#define KEYTABLE_MAX_EVENTS 32
#define KEYTABLE_MAX_BYTES  64

/* The modes of a case; see the table's header. */
#define KEYTABLE_CKM    0x01u /* DECCKM set */
#define KEYTABLE_KPAM   0x02u /* DECKPAM set */
#define KEYTABLE_NUM    0x04u /* Num Lock on before the first event */
#define KEYTABLE_CAPS   0x08u /* Caps Lock on before the first event */
#define KEYTABLE_TTY    0x10u /* the result is what a reader of the tty gets */
#define KEYTABLE_ICANON 0x20u /* with KEYTABLE_TTY: ICANON set */
#define KEYTABLE_ICRNL  0x40u /* with KEYTABLE_TTY: ICRNL set */
#define KEYTABLE_ISIG   0x80u /* with KEYTABLE_TTY: ISIG set */

enum keytable_kind {
    KEYTABLE_BYTES, /* the terminal sends bytes[0..nbytes), maybe none */
    KEYTABLE_LOCAL, /* the terminal acts on the key and sends nothing */
    KEYTABLE_SIGNAL /* the tty sends sig to its foreground process group */
};

/* A key of the 105-key keyboard. */
struct keytable_key {
    const char *name;    /* Linux's KEY_ name, lowercased, without KEY_ */
    unsigned short code; /* Linux's KEY_ number */
};

struct keytable_event {
    unsigned short code; /* Linux's KEY_ number */
    unsigned char press; /* 1 for a press, 0 for a release */
};

/* One case: the events from the given modes, and what they send in all. */
struct keytable_case {
    unsigned modes; /* KEYTABLE_CKM and the rest */
    int nevents;
    struct keytable_event events[KEYTABLE_MAX_EVENTS];
    enum keytable_kind kind;
    int sig; /* KEYTABLE_SIGNAL: Linux's number, 2 SIGINT, 3 SIGQUIT, 20 SIGTSTP */
    int nbytes;
    unsigned char bytes[KEYTABLE_MAX_BYTES];
    int line; /* the line of the table that gave the case */
};

/* A template event: a key, or a modifier class that a case resolves. */
struct keytable_tevent {
    short code;      /* Linux's KEY_ number, or -1 for a class */
    signed char cls; /* with code -1: 0 shift, 1 ctrl, 2 alt */
    char what;       /* '+' press, '-' release, 't' press and release */
};

/* A line of the table before its modifier classes are resolved. */
struct keytable_template {
    int nevents;
    struct keytable_tevent events[KEYTABLE_MAX_EVENTS];
    enum keytable_kind kind;
    int sig;
    int nbytes;
    unsigned char bytes[KEYTABLE_MAX_BYTES];
};

/* The reader's state; keytable_open() and keytable_init() set it up. */
struct keytable {
    FILE *f;
    int owned;                        /* keytable_close() closes f */
    const char *name;                 /* the table's name in messages */
    int line;                         /* the line last read */
    unsigned modes;                   /* from the last modes line */
    struct keytable_template tmpl[8]; /* the line being expanded */
    int ntmpl, ti;                    /* its templates, and the one being expanded */
    unsigned variant, nvariants;      /* the side choice of tmpl[ti], and their count */
    int tline;                        /* the line of the templates */
    char error[200];                  /* why keytable_next() returned -1 */
};

extern const struct keytable_key keytable_keys[];
extern const int keytable_nkeys;

/**
 * Open the table at `path` for keytable_next().
 *
 * Return 0, or -1 with the reason in t->error.
 */
int keytable_open(struct keytable *t, const char *path);

/**
 * Read a table from the open stream `f`, named `name` in messages; `f` stays
 * the caller's.
 */
void keytable_init(struct keytable *t, FILE *f, const char *name);

/**
 * Give the table's next case in `c`.
 *
 * Return 1 for a case, 0 at the end of the table, and -1 for a line the
 * reader refuses, with the table's name, the line and the reason in t->error.
 */
int keytable_next(struct keytable *t, struct keytable_case *c);

/** Close what keytable_open() opened. */
void keytable_close(struct keytable *t);

/** Return the code of the key named `name`, or -1 for none of the 105. */
int keytable_key_code(const char *name);

/** Return the name of the key `code`, or NULL for none of the 105. */
const char *keytable_key_name(unsigned code);

/**
 * Write `n` bytes into `buf` of `size` bytes as the table writes them, quoted,
 * truncated to fit and NUL-terminated.
 */
void keytable_quote(const unsigned char *bytes, int n, char *buf, size_t size);

/**
 * Write a line describing `c` into `buf` of `size` bytes, for a message: its
 * table line, its modes, its events and its result.
 */
void keytable_describe(const struct keytable_case *c, char *buf, size_t size);

#endif
