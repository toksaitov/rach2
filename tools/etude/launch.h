/*
 * launch.h: the parts that `etude launch`, the installed launcher, and
 * `etude make`, the make wrapper it puts first on PATH, share: the reading
 * of make's job flags and the start of a program in place of etude.
 */
#ifndef ETUDE_LAUNCH_H
#define ETUDE_LAUNCH_H

#include <stddef.h>

/* The job flags of a make command line, read as make reads them. */
struct launch_jobs {
    const char *unbounded; /* the count a -j or --jobs without one gives */
    const char *count;     /* the count of the last job flag, or "" */
    const char *jobs;      /* JOBS, or NULL where a JOBS= word is not read */
    int pending;           /* the last word was -j or --jobs */
    char **copies;         /* the words, which `count` and `jobs` point into */
    size_t ncopies;
};

/**
 * Read the job flags of MAKEFLAGS and of `argv`, which ends with a null
 * pointer, into `s`, whose `unbounded`, `count` and `jobs` the caller sets
 * first. The words of MAKEFLAGS's first line count up to `--`, and every
 * other line of MAKEFLAGS and of each argument is one word. A `-j` or
 * `--jobs` takes the next word as its count when that is a positive number.
 *
 * @return 0, or -1 when memory runs out; launch_jobs_free() frees the words.
 */
int launch_jobs_read(struct launch_jobs *s, char *const *argv);
void launch_jobs_free(struct launch_jobs *s);

/** Say whether `s` is a positive decimal number with no leading zero. */
int launch_number(const char *s);

/** Say whether `path` is absolute, as `/...` or `C:/...`. */
int launch_absolute(const char *path);

/**
 * Find `name` on PATH, as `command -v` does: the first `DIR/name` that is an
 * executable regular file, an empty entry being the working directory.
 *
 * @return 0 with the path in `out`, or -1.
 */
int launch_find(const char *name, char *out, size_t size);

/**
 * Run `argv` in place of etude: with execv(), or execvp() when `search` is
 * set, and on Windows, where a process cannot be replaced, as a child that
 * shares the standard streams, whose exit status the caller returns.
 * `who` names the command in a message.
 *
 * @return the status to exit with: the child's on Windows; 126 or 127 when
 *         the program cannot be started.
 */
int launch_replace(const char *who, char **argv, int search);

/**
 * Run `argv` as a child with the standard streams and wait for it.
 *
 * @return its exit status, 128 plus the signal that ended it, or 126 or 127
 *         when it cannot be started.
 */
int launch_child(const char *who, char **argv);

#endif
