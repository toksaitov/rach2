/*
 * status.c: the writer of a run's status record on POSIX systems, which
 * replaces the record whole by a rename so that a reader never sees part of
 * one.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#ifdef _WIN32
#include "../win/files.h"
#endif

#include "etude/status.h"

/* The names of the stages, as the history gives them, by enum etude_stage. */
static const char *const stage_names[] = {"queue",         "build",         "prepare", "transfer",
                                          "boot",          "execute",       "capture", "cleanup",
                                          "tooling_start", "one_time_setup"};

/** Say whether `s` is one field of a line: not empty, and without blanks. */
static int token(const char *s)
{
    return s != NULL && *s != '\0' && strpbrk(s, " \t\r\n") == NULL;
}

/** Say whether `s` fits on the rest of a line. */
static int line(const char *s)
{
    return s != NULL && strpbrk(s, "\r\n") == NULL;
}

/** Give a time of the monotonic clock as a time of the system's clock. */
static uint64_t on_epoch(uint64_t t, uint64_t now_us, uint64_t epoch_us)
{
    if (t >= now_us)
        return epoch_us + (t - now_us);
    return now_us - t < epoch_us ? epoch_us - (now_us - t) : 0;
}

void etude_status_watch(struct etude_status_attempt *a, const struct etude_watchdog *w,
                        uint64_t now_us, uint64_t epoch_us)
{
    const struct etude_progress *expired = etude_watchdog_expired(w);
    const struct etude_progress *p = expired != NULL ? expired : etude_watchdog_progress(w);

    a->waiting = 0;
    a->stage = p->stage;
    a->expired = expired != NULL;
    a->kind = etude_watchdog_due(w);
    a->progress_us = on_epoch(p->stage == ETUDE_STAGE_EXECUTE ? p->last_us : p->stage_started_us,
                              now_us, epoch_us);
    a->deadline_us = expired != NULL ? 0 : on_epoch(etude_watchdog_deadline(w), now_us, epoch_us);
    a->bound_us = etude_watchdog_bound(w);
}

/** Say whether every field of `s` can be written on its line. */
static int sound(const struct etude_status *s)
{
    size_t i;

    if (!token(s->run_id) || !token(s->ledger) || !line(s->args) || s->owner.pid <= 0)
        return 0;
    for (i = 0; i < s->nattempts; i++) {
        const struct etude_status_attempt *a = &s->attempts[i];

        if (!token(a->attempt_id) || !token(a->case_id) || !token(a->architecture))
            return 0;
        if (!a->waiting && ((size_t)a->stage >= sizeof(stage_names) / sizeof(stage_names[0]) ||
                            a->kind == ETUDE_EXPIRY_NONE))
            return 0;
    }
    return 1;
}

/** Write the lines of record `s` to `f`. */
static void lines(FILE *f, const struct etude_status *s)
{
    size_t i;

    fprintf(f, "%s\nowner %ld %llu %d\nrun %s\nledger %s\nargs %s\nwritten %llu\n",
            ETUDE_STATUS_MAGIC, s->owner.pid, (unsigned long long)s->owner.start,
            s->owner.start_known ? 1 : 0, s->run_id, s->ledger, s->args,
            (unsigned long long)s->written_us);
    for (i = 0; i < s->nattempts; i++) {
        const struct etude_status_attempt *a = &s->attempts[i];

        if (a->waiting) {
            fprintf(f, "wait %s %s %s %llu\n", a->attempt_id, a->case_id, a->architecture,
                    (unsigned long long)a->since_us);
            continue;
        }
        fprintf(f, "attempt %s %s %s %s %s %s %llu ", a->attempt_id, a->case_id, a->architecture,
                stage_names[a->stage], a->expired ? "expired" : "running",
                etude_expiry_name(a->kind), (unsigned long long)a->progress_us);
        if (a->expired)
            fprintf(f, "- %llu\n", (unsigned long long)a->bound_us);
        else
            fprintf(f, "%llu %llu\n", (unsigned long long)a->deadline_us,
                    (unsigned long long)a->bound_us);
    }
}

int etude_status_write(const char *path, const struct etude_status *s)
{
    char tmp[4096];
    FILE *f;
    int e = 0;

    if (!sound(s) || !line(path)) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.new", path) >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    f = fopen(tmp, "w");
    if (f == NULL)
        return -1;
    errno = 0;
    lines(f, s);
    if (fflush(f) != 0 || ferror(f))
        e = errno != 0 ? errno : EIO;
    if (fclose(f) != 0 && e == 0)
        e = errno;
    if (e == 0 && rename(tmp, path) != 0)
        e = errno;
    if (e != 0) {
        remove(tmp);
        errno = e;
        return -1;
    }
    return 0;
}

uint64_t etude_status_now(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < 0)
        return 0;
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}
