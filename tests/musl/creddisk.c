/**
 * creddisk: check that a credential replacement and the writer's lock reach
 * the disk, across three boots of one disk image.
 *
 * link-deps: utils/pwauth.c
 *
 * Every other test of `cred_rewrite` runs on the RAM module, where both of
 * its flushes do nothing and a library that dropped either would pass them
 * all. This boot can tell: the KRFFS is mounted from a disk through the page
 * cache (`rach2.fsdisk`), so anything left in the cache when the machine stops
 * is lost. Which boot this is, is read off `/credtarget`, and each one leaves
 * the disk in the state the next one reads. `tests/run cred-persist` boots
 * the image three times.
 *
 * The first boot replaces `/credtarget` under a transaction and ends it, then
 * takes a second transaction, replaces `/credalso`, and leaves through `_exit`
 * still holding it. That second half is a writer cut off where a power failure
 * would cut it off: no release and no flush after the one `cred_rewrite`
 * makes itself, so the flush after the rename is the only one that can take
 * the publish to the disk.
 *
 * The second boot is the reader of both. `/credtarget` and `/credalso` must
 * hold their replacements with the modes and owners they inherited and no
 * temporary file beside either, and the lock the cut-off writer left must still
 * be there, since that is what a killed writer leaves and what the next writer
 * must report, not break. It reports it, with a line naming the removal:
 * this kernel restarts pids at every boot, so a lock from a previous boot names
 * a pid that may well be live and unrelated, and an operator told only to wait
 * would wait forever. Then the probe does what that line says, takes a
 * transaction of its own, replaces `/credtarget` again and ends it.
 *
 * The third boot is that release read back: no lock at all. A release whose
 * removal stayed in the cache would leave one on every clean writer's reboot,
 * which is the same dead end reached by accident.
 *
 * The boots print "CREDDISK OK (wrote)", "CREDDISK OK (released)" and
 * "CREDDISK OK (read back)" in turn; any failure prints
 * "CREDDISK FAIL (detail)" and exits 1.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "pwauth.h"

/* The two staged files, root's, at modes 0600 and 0640, and the texts
 * `TARGET` has before the first boot, after it, and after the second. */
#define TARGET   "/credtarget"
#define ALSO     "/credalso"
#define BEFORE   "original\n"
#define AFTER    "replaced\n"
#define RELEASED "released\n"

/** Print the failure line and return 1, the status every case fails with. */
static int fail(const char *what)
{
    printf("CREDDISK FAIL (%s)\n", what);

    return 1;
}

/**
 * Read at most `size - 1` bytes of `path` into `buffer`, terminating it.
 * Returns the byte count, or -1 if the file could not be read.
 */
static ssize_t slurp(const char *path, char *buffer, size_t size)
{
    ssize_t n;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buffer, size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buffer[n] = '\0';

    return n;
}

/**
 * Count the temporary files of `path` in the root directory.
 *
 * A replacement writes through `<path>.tmp.<pid>`, so the count matches a
 * prefix: the pid in a name an earlier boot left belonged to a process of
 * that boot. A root directory that cannot be opened counts as one temporary
 * file, so that a failure to look is not taken for a clean disk.
 */
static int temps_left(const char *path)
{
    char prefix[64];
    struct dirent *entry;
    DIR *dir = opendir("/");
    int n = 0;

    snprintf(prefix, sizeof(prefix), "%s.tmp", path + 1);
    if (!dir) {
        return 1;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            n++;
        }
    }
    closedir(dir);

    return n;
}

/** True if `path` names something that exists. */
static int present(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/** True if `path` has exactly `text` at mode `mode`, owned by root. */
static int has(const char *path, const char *text, mode_t mode)
{
    char content[64];
    struct stat st;

    if (slurp(path, content, sizeof(content)) < 0 || strcmp(content, text) != 0) {
        return 0;
    }

    return stat(path, &st) == 0 && (st.st_mode & 07777) == mode && st.st_uid == 0 && st.st_gid == 0;
}

/**
 * Run the first boot: one replacement released, and one the writer is cut off
 * holding. Exits 0 on success, and returns 1 after printing a failure.
 *
 * The cut-off half leaves through `_exit`, so the release registered with
 * `atexit` never runs and no flush follows the one `cred_rewrite` makes. The
 * standard output is flushed by hand for the same reason.
 */
static int write_boot(void)
{
    if (cred_txn_begin("creddisk") != 0) {
        return fail("could not take the transaction the replacement runs under");
    }
    if (cred_rewrite(TARGET, AFTER, sizeof(AFTER) - 1) != CRED_PUBLISHED) {
        return fail("the replacement was not published and flushed");
    }
    if (cred_txn_end() != 0) {
        return fail("could not end the transaction the replacement ran under");
    }

    if (cred_txn_begin("creddisk") != 0) {
        return fail("could not take the transaction the cut-off writer holds");
    }
    if (cred_rewrite(ALSO, AFTER, sizeof(AFTER) - 1) != CRED_PUBLISHED) {
        return fail("the cut-off writer's replacement was not published and flushed");
    }
    printf("CREDDISK OK (wrote)\n");
    fflush(stdout);
    _exit(0);

    return 0;
}

/**
 * Run the second boot: both replacements must have reached the disk, and the
 * lock the cut-off writer left is reported and left in place by an acquire.
 * The lock is then removed by hand, as the report says, and `TARGET` replaced
 * again under a transaction that is released. Returns 0, or 1 after printing
 * the failure.
 */
static int read_boot(void)
{
    char report[512];
    int log;
    int saved;
    int taken;

    if (temps_left(TARGET) != 0 || temps_left(ALSO) != 0) {
        return fail("a temporary file outlived the boot that published");
    }
    if (!has(TARGET, AFTER, 0600)) {
        return fail("the released replacement did not survive the boot");
    }
    if (!has(ALSO, AFTER, 0640)) {
        return fail("the cut-off writer's replacement did not survive the boot");
    }
    if (!present(CRED_LOCK_PATH)) {
        return fail("the cut-off writer's lock did not survive the boot");
    }

    log = open("/credreport.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600);
    saved = dup(2);
    if (log < 0 || saved < 0 || dup2(log, 2) != 2) {
        return fail("could not capture what the refused acquire reports");
    }
    close(log);
    taken = cred_txn_begin_at("creddisk", CRED_LOCK_PATH, 0);
    dup2(saved, 2);
    close(saved);
    if (taken == 0) {
        cred_txn_end();

        return fail("an acquire took the lock a killed writer left");
    }
    if (!present(CRED_LOCK_PATH)) {
        return fail("a refused acquire removed the lock it reported");
    }
    if (slurp("/credreport.txt", report, sizeof(report)) < 0 || strstr(report, "remove") == NULL) {
        return fail("a lock left by a boot that is over was reported with no way out");
    }
    unlink("/credreport.txt");

    if (unlink(CRED_LOCK_PATH) != 0) {
        return fail("could not remove the stale lock by hand");
    }
    if (cred_txn_begin("creddisk") != 0) {
        return fail("could not take a transaction once the stale lock was gone");
    }
    if (cred_rewrite(TARGET, RELEASED, sizeof(RELEASED) - 1) != CRED_PUBLISHED) {
        return fail("the second replacement was not published and flushed");
    }
    if (cred_txn_end() != 0 || present(CRED_LOCK_PATH)) {
        return fail("ending the transaction did not drop the lock");
    }
    printf("CREDDISK OK (released)\n");

    return 0;
}

/**
 * Run the third boot: the release reached the disk, so no lock is there, and
 * the last replacement did too. Returns 0, or 1 after printing the failure.
 */
static int release_boot(void)
{
    if (present(CRED_LOCK_PATH)) {
        return fail("a released lock outlived the boot that released it");
    }
    if (temps_left(TARGET) != 0) {
        return fail("a temporary file outlived the boot that published");
    }
    if (!has(TARGET, RELEASED, 0600)) {
        return fail("the second replacement did not survive the boot");
    }
    printf("CREDDISK OK (read back)\n");

    return 0;
}

/** Run the boot that the text of `TARGET` names; 0, or 1 on a failure. */
int main(void)
{
    char content[64];

    if (slurp(TARGET, content, sizeof(content)) < 0) {
        return fail("the staged file is not there at all");
    }
    if (strcmp(content, BEFORE) == 0) {
        return write_boot();
    }
    if (strcmp(content, AFTER) == 0) {
        return read_boot();
    }
    if (strcmp(content, RELEASED) == 0) {
        return release_boot();
    }

    return fail("the staged file has none of the three texts this probe writes");
}
