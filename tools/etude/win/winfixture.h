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

/* winfixture.exe's directory, in UTF-8, where fixtures make their files. */
extern char scratch[];

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

/**
 * A running case is sampled with its processor time and working set, and an
 * ended one is not.
 */
int fx_exec_sample(void);

/** Waits of no time serve an attempt until it finishes, with its output. */
int fx_exec_poll(void);

/**
 * The ledger registers an attempt and its reported descendant, and recovery
 * keeps a claim while its case runs and frees it once the case's job ends.
 */
int fx_ledger(void);

/*
 * The tooling fixtures. Each runs etude-tooling, beside winfixture.exe,
 * against the stand-ins of winstand.exe, as a row of
 * tests/system/etude_tooling_test.sh does: a machine made, recorded and
 * reused; an image of another digest; a provision that fails; a job with its
 * inputs, a disk and its outputs; a job that exits 3; a run before the
 * machine is made; a hang past the deadline; QEMU's crash; a runner whose
 * status the volume contradicts; and QEMU's command line. The last requires
 * a job's step to have QEMU's processor time and working set.
 */
int fx_tooling_made(void);
int fx_tooling_digest(void);
int fx_tooling_provision(void);
int fx_tooling_job(void);
int fx_tooling_status(void);
int fx_tooling_unmade(void);
int fx_tooling_late(void);
int fx_tooling_crash(void);
int fx_tooling_lie(void);
int fx_tooling_line(void);
int fx_tooling_sample(void);

#endif
