/*
 * winfixture.h: what the files of winfixture share: the target's path, the
 * completion bound and the report of a failed check.
 */
#ifndef ETUDE_WINFIXTURE_H
#define ETUDE_WINFIXTURE_H

/* The completion bound of every wait, in milliseconds. */
#define BOUND 60000

/* The path of wintarget.exe, beside winfixture.exe, in UTF-8. */
extern char target[];

/**
 * Record why the current fixture failed, in the manner of printf.
 *
 * @return 1, the result of a failed fixture.
 */
int failed(const char *fmt, ...);

/** Fail the current fixture, with the message after COND, unless COND holds. */
#define CHECK(cond, ...)                \
    do {                                \
        if (!(cond))                    \
            return failed(__VA_ARGS__); \
    } while (0)

/** Attempts run at once through etude/exec.h keep their own results. */
int fx_exec_attempts(void);

/**
 * A refused case is stopped before it runs, and neither it nor a missing
 * program gives an attempt.
 */
int fx_exec_unrun(void);

#endif
