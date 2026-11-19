/* collection_lock.c: the native collection lock and its journal. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "collection.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * Write `text` to `path`, replacing it.
 *
 * Returns 1, or 0 when the file could not be written whole.
 */
static int write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    int ok;
    if (!fp)
        return 0;
    ok = fputs(text, fp) >= 0;
    return !fclose(fp) && ok;
}

/**
 * Read the first line of `path` into `line` of `size` bytes, without its
 * newline.
 *
 * Returns 1, or 0 when it cannot be read or is empty.
 */
static int first_line(const char *path, char *line, size_t size)
{
    FILE *fp = fopen(path, "r");
    int ok = fp && fgets(line, (int)size, fp) != NULL;
    if (fp)
        fclose(fp);
    if (ok)
        line[strcspn(line, "\r\n")] = 0;
    return ok && *line;
}

/**
 * Format this process's owner record into `out` of `size` bytes: its PID,
 * its start in clock ticks since boot (field 22 of /proc/self/stat) and the
 * kernel's boot_id, the fields of the deployment's controller receipt, each
 * `-` where the system gives none, separated by one space.
 */
static void owner_record(char *out, size_t size)
{
    char self[1024], start[32] = "-", boot[128] = "-";
    if (first_line("/proc/self/stat", self, sizeof(self))) {
        char *at = strrchr(self, ')');
        int field = 2;
        for (; at && field < 22; field++)
            at = strchr(at + 1, ' ');
        if (at && sscanf(at + 1, "%31[0-9]", start) != 1)
            strcpy(start, "-");
    }
    if (!first_line("/proc/sys/kernel/random/boot_id", boot, sizeof(boot)))
        strcpy(boot, "-");
    snprintf(out, size, "%ld %s %s", (long)getpid(), start, boot);
}

/**
 * Open STATE/collection.serial in the state directory `state` and hold
 * flock(2) on it.
 *
 * The setup of `native-deploy run` and its cleanup hold the same flock,
 * through flock(1), while they read a lock's owner, its journal or a
 * controller claim and remove one, so that none of them removes a lock
 * taken, or reads one released, after its reading. Returns the descriptor,
 * which the caller closes to drop the flock, or -1.
 */
static int serial_hold(const char *state)
{
    char serial[4200];
    int fd;
    snprintf(serial, sizeof(serial), "%s/collection.serial", state);
    fd = open(serial, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "native: cannot open %s: %s\n", serial, strerror(errno));
        return -1;
    }
    while (flock(fd, LOCK_EX))
        if (errno != EINTR) {
            fprintf(stderr, "native: cannot lock %s: %s\n", serial, strerror(errno));
            close(fd);
            return -1;
        }
    return fd;
}

/**
 * Write STATE/collection.journal in the state directory `state` before a
 * change of the lock: `what`, the change, and `record`, the owner record of
 * the lock it makes or removes.
 *
 * The caller holds serial_hold's flock and removes the journal once the
 * change is whole, before it drops the flock, so that a journal seen under
 * the flock was left by a process stopped midway. The next run's setup then
 * removes the lock when it has no owner record or the one the journal names,
 * and the journal. Returns 1, or 0 when it cannot be written.
 */
static int journal_begin(const char *state, const char *what, const char *record)
{
    char path[4200], text[512];
    snprintf(path, sizeof(path), "%s/collection.journal", state);
    snprintf(text, sizeof(text), "%s %s\n", what, record);
    if (write_text(path, text))
        return 1;
    fprintf(stderr, "native: cannot write %s\n", path);
    unlink(path);
    return 0;
}

/**
 * Remove the journal of journal_begin in the state directory `state`.
 *
 * Returns 1, or 0 when it remains.
 */
static int journal_end(const char *state)
{
    char path[4200];
    snprintf(path, sizeof(path), "%s/collection.journal", state);
    if (!unlink(path) || errno == ENOENT)
        return 1;
    fprintf(stderr, "native: cannot remove %s: %s\n", path, strerror(errno));
    return 0;
}

/**
 * Remove the lock directory `lock` and its owner record.
 *
 * Returns 1 when the lock is gone, or 0.
 */
static int lock_remove(const char *lock)
{
    char path[4200];
    snprintf(path, sizeof(path), "%s/owner", lock);
    if (unlink(path) && errno != ENOENT)
        return 0;
    return !rmdir(lock) || errno == ENOENT;
}

/**
 * Take the collection lock `lock`, a directory holding `owner`, this
 * process's owner record, in the state directory `state`.
 *
 * Under serial_hold's flock, a lock or a journal already there is refused
 * before anything is written, so that a contender never journals another
 * collection's lock. The making is journalled, since a stop between the
 * mkdir and the owner's write would leave a lock with no owner record, which
 * a setup refuses; a failure undoes it, and keeps the journal for the setup
 * while the undoing has not removed the lock. Returns 1, or 0 with no lock
 * of this process left that has no journal.
 */
int nc_lock_take(const char *state, const char *lock)
{
    char journal[4200], owner[4200], record[256], line[300];
    struct stat st;
    int fd = serial_hold(state), ok = 0;
    if (fd < 0)
        return 0;
    snprintf(journal, sizeof(journal), "%s/collection.journal", state);
    snprintf(owner, sizeof(owner), "%s/owner", lock);
    owner_record(record, sizeof(record));
    snprintf(line, sizeof(line), "%s\n", record);
    if (!lstat(lock, &st) || errno != ENOENT)
        fprintf(stderr, "native: another collection owns %s\n", lock);
    else if (!lstat(journal, &st) || errno != ENOENT)
        fprintf(stderr, "native: %s: a stopped change of %s; a deployment's setup clears it\n",
                journal, lock);
    else if (journal_begin(state, "make", record)) {
        if (mkdir(lock, 0700)) {
            fprintf(stderr, "native: cannot make %s: %s\n", lock, strerror(errno));
            journal_end(state);
        } else if (!write_text(owner, line) || !journal_end(state)) {
            fprintf(stderr, "native: cannot record the owner of %s\n", lock);
            if (lock_remove(lock))
                journal_end(state);
        } else
            ok = 1;
    }
    close(fd);
    return ok;
}

/**
 * Remove the collection lock `lock` in the state directory `state` and its
 * owner record under serial_hold's flock, journalled with the owner record
 * as nc_lock_take's change is.
 *
 * A removal that fails keeps the journal, so that the next run's setup
 * removes the lock. Returns 0, or -1 when the lock remains.
 */
int nc_lock_release(const char *state, const char *lock)
{
    char owner[4200], record[256] = "-";
    int fd = serial_hold(state), status = -1;
    if (fd < 0)
        return -1;
    snprintf(owner, sizeof(owner), "%s/owner", lock);
    if (!first_line(owner, record, sizeof(record)))
        strcpy(record, "-");
    if (journal_begin(state, "release", record) && lock_remove(lock) && journal_end(state))
        status = 0;
    close(fd);
    return status;
}
