/**
 * The credential library and `su` in the guest, through the ported musl.
 *
 * link-deps: utils/pwauth.c
 *
 * The host test covers what is pure. This covers what is not: a real
 * kernel, a real `/etc`, two processes racing for one transaction, a real
 * terminal, and the setuid-root `su` staged beside it. Its cases, in the order
 * they run:
 *
 * The salt draw, first, as the control the no-entropy run inverts: on this
 * boot's `-cpu max` the mint must succeed, produce `PWAUTH_SALT_LEN`
 * characters of the `crypt(3)` alphabet, and produce a different salt the
 * second time.
 *
 * The replacement, where the guest can say what the host cannot: the file is
 * replaced whole, its mode and owner are the original's (root and uid 1000
 * differ here, so the inheritance is a real assertion rather than a tautology),
 * no temporary file is left behind, and a reader holding a descriptor across
 * the rename goes on reading the file it opened, whole. That last one is the
 * guarantee the whole reader-never-locks rule rests on. A second row has a
 * set-id bit, which only this test can: the owner must be set before the mode
 * because a chown clears those bits, and both an owner the writer is not and a
 * set-id bit are needed to see it.
 *
 * A fork under the transaction, since the state is file-scope and a child
 * inherits it while holding nothing: a child that exits, a child a signal
 * kills, and a child that calls the three entry points must all leave the
 * parent's lock exactly where it is. Then a lock cleared out from under a live
 * holder, which is the same rule turned on the library: with another writer's
 * lock at the name, neither the publish nor the release may touch it, and the
 * recorded pid is the only thing that can tell the two files apart, since
 * `st_ino` here is a hash of the name.
 *
 * The interleave, the case no `O_EXCL` on a temporary file can pass: two
 * children read and rewrite one file, ordered by a pipe so the second begins
 * its transaction while the first is asleep inside its own. Both lines must
 * survive. Without the lock the second reads the pre-image and its rename
 * destroys the first one's line, which is the lost update the transaction
 * exists to prevent; the second's acquire is also timed, since a lock that
 * never blocked could pass on ordering luck alone.
 *
 * The recovery rules, against the real `/etc/.pwlock` and the compiled-in retry
 * budget, each one an assertion that nothing was broken: a lock naming a reaped
 * child's pid is refused with the file still there and its bytes unchanged, the
 * same lock with its modification time pushed back an hour is refused exactly
 * as the fresh one was (the assertion the deleted age rule fails), and the
 * probe's own removal of the file lets the next acquire succeed. Waiting out
 * that budget twice is most of this probe's running time and is deliberate: the
 * bound itself is checked cheaply on the host, where the budget is a parameter.
 *
 * The stop and continue cycle, which needs a real terminal and so runs over a
 * pseudo-terminal rather than this probe's own console: a child in the pty's
 * foreground group reads a password, and the parent drives the master. Nothing
 * typed comes back while the prompt is up, a `^Z` stops the child with the
 * terminal's modes restored (proved by typing while it is stopped and seeing
 * the echo), a continue re-applies echo-off and reprints the prompt, and the
 * child exits 0 only if the password it read is exactly what was typed after
 * the continue: everything typed before it was flushed, and none of it became
 * a password byte.
 *
 * Then the two things one keystroke is not. A suspend character with bytes
 * behind it in the same write, which the discipline does not flush and the read
 * returns before the stop is served; and a continue that arrives without the
 * terminal, which is a shell's `bg`, where re-applying echo-off and reprinting
 * would silence a terminal that belongs to the shell.
 *
 * Then a standard error that is a pipe with no reader, which this kernel
 * answers
 * with a `SIGPIPE` whose default action is death: at the prompt that death
 * would
 * leave the console with echo off, and under a transaction it would leave the
 * lock file in `/etc`, since no `atexit` runs on a signal.
 *
 * Then `su` itself, from a uid-1000 caller with the known password on its
 * standard input: the non-login run, whose expectations are the paired half
 * that makes the login run mean something; a wrong password refused with the
 * caller's identity unchanged; and the login run, which must land in the
 * target's home under a leading-dash `argv[0]`. Both runs are made from a
 * working directory of `/musl` with a variable of the caller's own in the
 * environment, so the login run proves the working directory changed and the
 * non-login run proves it did not, and neither may carry the caller's variable
 * through.
 *
 * It snapshots `/etc/passwd`, `/etc/shadow` and `/etc/group` at entry and puts
 * them back byte for byte, removes its own fixtures, and asserts before it does
 * that no lock and no temporary file is left anywhere it touched.
 *
 * Prints "PWAUTHMUSL OK" on success and "PWAUTHMUSL FAIL (detail)" otherwise.
 * Linked against the ported musl and the credential library.
 */

#define _GNU_SOURCE /* setresuid, getresuid */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pwauth.h"

#define SALT_ALPHABET "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

/* The committed test credential of the image's `probe` account (uid 1000,
 * `/idreport` as its shell). Root's own password is a different one, which is
 * what makes it the wrong password to offer here. */
#define PROBE_PASSWORD "probepass"
#define ROOT_PASSWORD  "rach2"

/* The five variables su builds for the target, and the caller's own, which must
 * not reach it. */
#define SU_ENVIRONMENT                                                 \
    " env=PATH=/bin:/usr/bin env=HOME=/home/probe env=SHELL=/idreport" \
    " env=USER=probe env=LOGNAME=probe"
#define IDS_1000         "uid=1000 euid=1000 suid=1000 gid=1000 egid=1000 sgid=1000"
#define EXPECT_LOGIN     IDS_1000 " arg0=-idreport cwd=/home/probe" SU_ENVIRONMENT
#define EXPECT_NON_LOGIN IDS_1000 " arg0=/idreport cwd=/musl" SU_ENVIRONMENT

/* A uid-1000 caller that came back from a refused `su` with its identity
 * changed, and one whose `su` died of a signal: both are the run's own failure
 * rather than a status su chose, so they are kept clear of the small statuses
 * su and idreport use. */
#define CALLER_BROKEN 127
#define CALLER_SIGNAL 126

static const char *Fixtures[] = {"/credtxn.txt", "/credbusy.txt", "/credmode.txt", "/credlog.txt",
                                 "/credlost.txt"};

/* No file any of these is replaced through may be left behind: a temporary file
 * outlives a replacement only after a crash. A replacement writes through
 * `<path>.tmp.<pid>`, so each of these is a prefix to sweep for and not a name
 * to look up: a temporary left by one of this probe's children has that
 * child's pid. The lock is checked beside them, since a lock outlives its
 * holder
 * only after a kill. */
static const char *Replaced[] = {"/etc/passwd",  "/etc/shadow",   "/etc/group",
                                 "/credtxn.txt", "/credmode.txt", "/credlost.txt"};

static const char *Databases[] = {"/etc/passwd", "/etc/shadow", "/etc/group"};
static char Snapshot[3][512];
static ssize_t Snapshot_len[3];

/** Print the failure line and return -1, the status every case fails with. */
static int fail(const char *what)
{
    printf("PWAUTHMUSL FAIL (%s)\n", what);

    return -1;
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
 * Write into `out` the shadow `text` with `user`'s hash field replaced by
 * `hash`. Returns 0, or -1 when it would not fit.
 */
static int replace_field(const char *text, const char *user, const char *hash, char *out,
                         size_t size)
{
    size_t len = strlen(user);
    const char *line = text;
    size_t off = 0;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);
        int mine = span > len && strncmp(line, user, len) == 0 && line[len] == ':';

        if (mine) {
            const char *rest = memchr(line + len + 1, ':', span - len - 1);
            size_t tail = rest ? span - (size_t)(rest - line) : 0;

            if (off + len + 1 + strlen(hash) + tail + 2 >= size) {
                return -1;
            }
            memcpy(out + off, line, len + 1);
            off += len + 1;
            memcpy(out + off, hash, strlen(hash));
            off += strlen(hash);
            if (rest) {
                memcpy(out + off, rest, tail);
                off += tail;
            }
        } else {
            if (off + span + 2 >= size) {
                return -1;
            }
            memcpy(out + off, line, span);
            off += span;
        }
        if (end) {
            out[off++] = '\n';
        } else {
            break;
        }
        line = end + 1;
    }
    out[off] = '\0';

    return 0;
}

/**
 * Write `text` to `path` at mode `mode`, replacing whatever was there. Returns
 * 0 on success and -1 otherwise.
 */
static int spill(const char *path, const char *text, mode_t mode)
{
    size_t len = strlen(text);
    ssize_t n = 0;
    int fd;

    unlink(path);
    fd = open(path, O_CREAT | O_WRONLY, mode);
    if (fd < 0) {
        return -1;
    }
    if (len > 0) {
        n = write(fd, text, len);
    }
    close(fd);

    return (n == (ssize_t)len) ? 0 : -1;
}

/** True if `path` names something that exists. */
static int present(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/**
 * How many temporary files of `path`'s the directory holding it still has.
 *
 * A replacement writes through `<path>.tmp.<pid>`, so no single name answers
 * whether the directory is clean: a temporary left behind by one of this
 * probe's
 * children is named for that child. This counts every name that begins the way
 * one of `path`'s temporaries does.
 *
 * `path` must be absolute, and a relative one counts as one temporary rather
 * than none, as does a directory that cannot be opened: a question this helper
 * could not answer fails the probe instead of reporting a directory it never
 * looked in as clean.
 */
static int temps_left(const char *path)
{
    char directory[64];
    char prefix[64];
    const char *slash = strrchr(path, '/');
    struct dirent *entry;
    DIR *dir;
    int n = 0;

    if (slash == NULL) {
        return 1;
    }
    snprintf(directory, sizeof(directory), "%.*s", (int)(slash == path ? 1 : slash - path), path);
    snprintf(prefix, sizeof(prefix), "%s.tmp", slash + 1);
    dir = opendir(directory);
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

/** How many times `needle` occurs in `haystack`, without overlaps. */
static int occurrences(const char *haystack, const char *needle)
{
    const char *p = haystack;
    int n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }

    return n;
}

/** Sleep `ms` milliseconds, the probe's one way of letting time pass. */
static void nap(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/**
 * End the transaction, if one is held, and leave the process.
 *
 * A forked child leaves through `_exit`, so the release registered with
 * `atexit` never runs; dropping the lock explicitly is what keeps a child's
 * failure from leaving one behind for the next case to trip over.
 */
static void child_exit(int code)
{
    cred_txn_end();
    _exit(code);
}

/**
 * The positive control the no-entropy run inverts: a salt is minted, it is the
 * declared length, every character is in the alphabet, and a second draw
 * differs.
 */
static int check_salt(void)
{
    char first[PWAUTH_SALT_LEN + 1];
    char second[PWAUTH_SALT_LEN + 1];
    size_t i;

    if (pwauth_mint_salt(first, sizeof(first)) != 0) {
        return fail("mint_salt refused on a CPU that has trusted entropy");
    }
    if (strlen(first) != PWAUTH_SALT_LEN) {
        return fail("mint_salt returned a salt of the wrong length");
    }
    for (i = 0; i < PWAUTH_SALT_LEN; i++) {
        if (strchr(SALT_ALPHABET, first[i]) == NULL) {
            return fail("mint_salt returned a character outside the alphabet");
        }
    }
    if (pwauth_mint_salt(second, sizeof(second)) != 0) {
        return fail("mint_salt refused the second draw");
    }
    if (strcmp(first, second) == 0) {
        return fail("two minted salts are identical");
    }

    return 0;
}

/**
 * The replacement in the guest: the whole file, the original's mode and owner,
 * no temporary file left, and a reader that opened before the rename still
 * reading what it opened.
 *
 * The reader is the assertion the library's whole reader-never-locks rule rests
 * on. It has a descriptor across the rename and must read the old file
 * entire, not a prefix of it and not a mixture of the two.
 *
 * The second row is the one place in the tree where the set-id rule and the
 * replacement idiom meet, and it needs both a set-id bit and an owner the
 * writer is not, so it can only be made here: a `chown` clears those bits, so a
 * replacement that took the mode before the owner would publish 0640 where the
 * original was 04640, and publish it quietly.
 */
static int check_rewrite(void)
{
    static const char before[] = "probe:x:1000:1000:probe:/home/probe:/idreport\n";
    static const char after[] = "probe:!:1000:1000:probe:/home/probe:/idreport\n";
    char buffer[256];
    struct stat st;
    int reader;
    int held;

    if (spill("/credmode.txt", before, 0640) != 0 || chown("/credmode.txt", 1000, 1000) != 0 ||
        chmod("/credmode.txt", 0640) != 0) {
        return fail("could not stage the file the replacement inherits from");
    }
    reader = open("/credmode.txt", O_RDONLY);
    if (reader < 0) {
        return fail("could not open the file the reader holds across the rename");
    }

    if (cred_txn_begin("pwauthmusl") != 0) {
        close(reader);

        return fail("could not take the transaction the replacement runs under");
    }
    held = cred_rewrite("/credmode.txt", after, sizeof(after) - 1);
    cred_txn_end();
    if (held != 0) {
        close(reader);

        return fail("cred_rewrite refused a replacement it should have published");
    }

    if (slurp("/credmode.txt", buffer, sizeof(buffer)) != (ssize_t)sizeof(after) - 1 ||
        strcmp(buffer, after) != 0) {
        close(reader);

        return fail("the published file is not the text that was written");
    }
    if (stat("/credmode.txt", &st) != 0 || (st.st_mode & 07777) != 0640 || st.st_uid != 1000 ||
        st.st_gid != 1000) {
        close(reader);

        return fail("the replacement did not inherit the original's mode and owner");
    }
    if (temps_left("/credmode.txt") != 0) {
        close(reader);

        return fail("the replacement left its temporary file behind");
    }

    buffer[0] = '\0';
    {
        ssize_t n = read(reader, buffer, sizeof(buffer) - 1);

        close(reader);
        if (n != (ssize_t)sizeof(before) - 1) {
            return fail("a reader across the rename did not read the old file whole");
        }
        buffer[n] = '\0';
    }
    if (strcmp(buffer, before) != 0) {
        return fail("a reader across the rename read something other than what it opened");
    }

    if (spill("/credmode.txt", before, 0640) != 0 || chown("/credmode.txt", 1000, 1000) != 0 ||
        chmod("/credmode.txt", 04640) != 0) {
        return fail("could not stage the set-id file the replacement inherits from");
    }
    if (cred_txn_begin("pwauthmusl") != 0) {
        return fail("could not take the transaction the set-id replacement runs under");
    }
    held = cred_rewrite("/credmode.txt", after, sizeof(after) - 1);
    cred_txn_end();
    if (held != CRED_PUBLISHED) {
        return fail("cred_rewrite refused a set-id file it should have published");
    }
    if (stat("/credmode.txt", &st) != 0 || (st.st_mode & 07777) != 04640 || st.st_uid != 1000 ||
        st.st_gid != 1000) {
        return fail("the replacement did not inherit the original's set-id bit and owner");
    }

    return 0;
}

/**
 * A `fork` under a held transaction, at the real lock, with the real signals.
 *
 * The state is file-scope, so a child inherits it while holding nothing: the
 * lock names its parent. Both ordinary ways out of such a child ran the release
 * once, and one keystroke reaches them, since `^C` at a terminal goes to every
 * process in the foreground group. So a child that exits and a child that a
 * signal kills must leave the lock where it is, and the three entry points must
 * all decline to act on a transaction that is not the child's. The child's own
 * acquire is the `_at` form only to spend no time waiting: what it must show is
 * which refusal it gets, not that the budget is real, which the cases below
 * spend twenty seconds proving.
 */
static int check_fork(void)
{
    char before[64];
    char after[64];
    char content[128];
    pid_t child;
    ssize_t n;
    int status;

    unlink("/etc/.pwlock");
    if (spill("/credlost.txt", "before the fork\n", 0600) != 0) {
        return fail("could not stage the file the fork cases guard");
    }
    if (cred_txn_begin("pwauthmusl") != 0) {
        return fail("could not take the transaction the fork cases run under");
    }
    n = slurp("/etc/.pwlock", before, sizeof(before));

    child = fork();
    if (child == 0) {
        exit(0); /* the release registered with atexit runs here */
    }
    if (child < 0 || waitpid(child, &status, 0) != child) {
        cred_txn_end();

        return fail("could not fork the child that exits under its parent's transaction");
    }
    if (!present("/etc/.pwlock")) {
        cred_txn_end();

        return fail("a child's exit removed its parent's lock");
    }

    child = fork();
    if (child == 0) {
        for (;;) {
            pause(); /* the death handler the transaction installed is inherited */
        }
    }
    if (child < 0) {
        cred_txn_end();

        return fail("could not fork the child a signal kills under its parent's transaction");
    }
    nap(200);
    kill(child, SIGINT);
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGINT) {
        cred_txn_end();

        return fail("a child under its parent's transaction did not die of the signal sent");
    }
    if (!present("/etc/.pwlock")) {
        cred_txn_end();

        return fail("a child's death removed its parent's lock");
    }

    child = fork();
    if (child == 0) {
        int wrong = 0;

        if (cred_txn_end() == 0) {
            wrong |= 1;
        }
        if (cred_rewrite("/credlost.txt", "usurped\n", 8) != CRED_UNCHANGED) {
            wrong |= 2;
        }
        if (cred_txn_begin_at("pwauthmusl", CRED_LOCK_PATH, 0) == 0) {
            wrong |= 4;
        }
        _exit(wrong);
    }
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        cred_txn_end();

        return fail("could not fork the child that tries the transaction it inherited");
    }
    if (WEXITSTATUS(status) != 0) {
        printf("pwauthmusl: the child acted on its parent's transaction: %d\n",
               WEXITSTATUS(status));
        cred_txn_end();

        return fail("a child acted on a transaction it does not hold");
    }
    if (slurp("/credlost.txt", content, sizeof(content)) < 0 ||
        strcmp(content, "before the fork\n") != 0) {
        cred_txn_end();

        return fail("a child replaced the file its parent's transaction protects");
    }
    if (slurp("/etc/.pwlock", after, sizeof(after)) != n || memcmp(before, after, (size_t)n) != 0) {
        cred_txn_end();

        return fail("a child rewrote the lock its parent holds");
    }

    if (cred_txn_end() != 0 || present("/etc/.pwlock")) {
        return fail("ending the transaction the fork cases held did not drop the lock");
    }

    return 0;
}

/**
 * A lock cleared out from under a live holder: neither the publish nor the
 * release may act on the file that took its place.
 *
 * This is the second reason a lock is never broken, turned on the library
 * itself. Root removing a lock the library reported as stale, and the next
 * writer creating its own at the same name, is the whole of the setup, and an
 * unconditional unlink at the end of the first transaction would then take the
 * second writer's lock, and the third writer's after that. Nothing but the
 * recorded pid can tell the two files apart here, which the first assertion
 * shows: `st_ino` on this file system is a hash of the name, so two files that
 * carried one name in turn are indistinguishable by it.
 */
static int check_lost_lock(void)
{
    char before[64];
    char after[64];
    char content[128];
    char body[64];
    struct stat first;
    struct stat second;
    pid_t reaped;
    ssize_t n;
    int status;
    int rc;

    unlink("/etc/.pwlock");
    if (spill("/credlost.txt", "the file a lost lock protects\n", 0600) != 0) {
        return fail("could not stage the file the lost-lock case guards");
    }
    if (spill("/etc/.pwlock", "1\n", 0600) != 0 || stat("/etc/.pwlock", &first) != 0) {
        return fail("could not stage the first of two locks at one name");
    }
    unlink("/etc/.pwlock");
    if (spill("/etc/.pwlock", "2\n", 0600) != 0 || stat("/etc/.pwlock", &second) != 0) {
        return fail("could not stage the second of two locks at one name");
    }
    if (first.st_ino != second.st_ino) {
        return fail("two files at one name have distinct inodes: bind the removal to one");
    }
    unlink("/etc/.pwlock");

    if (cred_txn_begin("pwauthmusl") != 0) {
        return fail("could not take the transaction the lost-lock case runs under");
    }
    reaped = fork();
    if (reaped == 0) {
        _exit(0);
    }
    if (reaped < 0 || waitpid(reaped, &status, 0) != reaped) {
        cred_txn_end();

        return fail("could not mint a pid for the writer that takes the name");
    }
    snprintf(body, sizeof(body), "%d\n", (int)reaped);
    unlink("/etc/.pwlock");
    if (spill("/etc/.pwlock", body, 0600) != 0) {
        cred_txn_end();

        return fail("could not stand another writer's lock at the name");
    }
    n = slurp("/etc/.pwlock", before, sizeof(before));

    rc = cred_rewrite("/credlost.txt", "usurped\n", 8);
    if (rc != CRED_UNCHANGED) {
        cred_txn_end();

        return fail("a replacement published with the transaction's lock gone");
    }
    if (slurp("/credlost.txt", content, sizeof(content)) < 0 ||
        strcmp(content, "the file a lost lock protects\n") != 0) {
        cred_txn_end();

        return fail("a replacement with the transaction's lock gone changed the target");
    }
    if (temps_left("/credlost.txt") != 0) {
        cred_txn_end();

        return fail("a replacement refused for a lost lock left its temporary file behind");
    }

    if (cred_txn_end() != 0) {
        return fail("cred_txn_end refused the transaction the lost-lock case held");
    }
    if (!present("/etc/.pwlock")) {
        return fail("a release removed a lock another writer created at the same name");
    }
    if (slurp("/etc/.pwlock", after, sizeof(after)) != n || memcmp(before, after, (size_t)n) != 0) {
        unlink("/etc/.pwlock");

        return fail("a release rewrote another writer's lock");
    }
    unlink("/etc/.pwlock");

    return 0;
}

/**
 * The first writer: take the transaction, announce it, hold it while a second
 * writer tries, then read, append and publish.
 *
 * The byte on the pipe is what orders the two, and it goes out only once the
 * lock is held, so the second writer is guaranteed to find it there. The pipe
 * closes when this process leaves whatever happens, so a failure here is a
 * refused acquire in the second writer rather than a wait with no end.
 */
static void interleave_first(int notify)
{
    char buffer[256];
    ssize_t n;

    if (cred_txn_begin("pwauthmusl") != 0) {
        child_exit(11);
    }
    if (write(notify, "x", 1) != 1) {
        child_exit(12);
    }
    sleep(2);
    n = slurp("/credtxn.txt", buffer, sizeof(buffer) - 8);
    if (n < 0) {
        child_exit(13);
    }
    strcat(buffer, "first\n");
    if (cred_rewrite("/credtxn.txt", buffer, strlen(buffer)) != 0) {
        child_exit(14);
    }
    child_exit(cred_txn_end() == 0 ? 0 : 15);
}

/**
 * The second writer: block until the first holds the transaction, then take it,
 * read, append and publish.
 *
 * Its acquire is timed, because ordering alone would also come out right by
 * luck; it must really have waited. Its diagnostics go to a file so the parent
 * can check that a wait is announced once and not once per retry.
 */
static void interleave_second(int ordered)
{
    char buffer[256];
    time_t started;
    ssize_t n;
    int log = open("/credbusy.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);

    if (log < 0 || dup2(log, 2) != 2) {
        child_exit(21);
    }
    close(log);
    if (read(ordered, buffer, 1) != 1) {
        child_exit(22);
    }
    started = time(NULL);
    if (cred_txn_begin("pwauthmusl") != 0) {
        child_exit(23);
    }
    if (time(NULL) - started < 1) {
        child_exit(24);
    }
    n = slurp("/credtxn.txt", buffer, sizeof(buffer) - 8);
    if (n < 0) {
        child_exit(25);
    }
    strcat(buffer, "second\n");
    if (cred_rewrite("/credtxn.txt", buffer, strlen(buffer)) != 0) {
        child_exit(26);
    }
    child_exit(cred_txn_end() == 0 ? 0 : 27);
}

/**
 * The interleave: two writers read and rewrite one file, and both their lines
 * must survive.
 */
static int check_interleave(void)
{
    char buffer[256];
    int ordering[2];
    pid_t first, second;
    int status;

    if (spill("/credtxn.txt", "original\n", 0644) != 0) {
        return fail("could not stage the file the two writers rewrite");
    }
    if (pipe(ordering) != 0) {
        return fail("could not make the pipe that orders the two writers");
    }

    first = fork();
    if (first == 0) {
        close(ordering[0]);
        interleave_first(ordering[1]);
    }
    second = fork();
    if (second == 0) {
        close(ordering[1]);
        interleave_second(ordering[0]);
    }
    close(ordering[0]);
    close(ordering[1]);
    if (first < 0 || second < 0) {
        return fail("could not fork the two writers");
    }

    if (waitpid(first, &status, 0) != first || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the first writer did not finish its transaction cleanly");
    }
    if (waitpid(second, &status, 0) != second || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the second writer did not wait for the transaction and publish");
    }

    if (slurp("/credtxn.txt", buffer, sizeof(buffer)) < 0 ||
        strcmp(buffer, "original\nfirst\nsecond\n") != 0) {
        return fail("an update was lost: the file does not hold both writers' lines");
    }
    if (slurp("/credbusy.txt", buffer, sizeof(buffer)) < 0 ||
        occurrences(buffer, "credential database busy, waiting") != 1) {
        return fail("a writer that waited did not announce it exactly once");
    }

    return 0;
}

/**
 * One refusal against a lock this probe wrote at the real `/etc/.pwlock`: the
 * acquire fails, the file is still there with its bytes unchanged, and the
 * report says which lock it is and what to do about it.
 *
 * The acquire waits out the compiled-in retry budget, which is the point: this
 * is the path a real writer takes, with the path and the budget the shipped
 * programs carry rather than parameters a test chose.
 */
static int check_refusal(const char *what, const char *body, const char *expected)
{
    char before[128];
    char after[128];
    char report[512];
    ssize_t n;
    int log;
    int saved;
    int taken;

    if (spill("/etc/.pwlock", body, 0600) != 0) {
        return fail("could not write the lock file a refusal case needs");
    }
    n = slurp("/etc/.pwlock", before, sizeof(before));

    log = open("/credlog.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    saved = dup(2);
    if (log < 0 || saved < 0 || dup2(log, 2) != 2) {
        return fail("could not capture what a refused acquire reports");
    }
    close(log);
    taken = cred_txn_begin("pwauthmusl");
    dup2(saved, 2);
    close(saved);
    if (taken == 0) {
        cred_txn_end();

        return fail(what);
    }

    if (slurp("/credlog.txt", report, sizeof(report)) < 0) {
        return fail("a refused acquire reported nothing at all");
    }
    if (occurrences(report, "credential database busy, waiting") != 1) {
        return fail("an acquire that waited did not announce it exactly once");
    }
    if (strstr(report, expected) == NULL || strstr(report, "remove it as root and retry") == NULL) {
        return fail("a stale lock was not reported as one to remove by hand");
    }
    if (!present("/etc/.pwlock")) {
        return fail("a refused acquire removed the lock file");
    }
    if (slurp("/etc/.pwlock", after, sizeof(after)) != n || memcmp(before, after, (size_t)n) != 0) {
        return fail("a refused acquire rewrote the lock file it reported");
    }

    return 0;
}

/**
 * The recovery rules at the real lock path, and the control that makes them
 * mean something: with the file gone, the same acquire succeeds.
 */
static int check_recovery(void)
{
    char body[64];
    char expected[64];
    pid_t reaped;
    int status;

    unlink("/etc/.pwlock");
    reaped = fork();
    if (reaped == 0) {
        _exit(0);
    }
    if (reaped < 0 || waitpid(reaped, &status, 0) != reaped) {
        return fail("could not mint a reaped pid for the stale-lock case");
    }
    snprintf(body, sizeof(body), "%d\n", (int)reaped);
    snprintf(expected, sizeof(expected), "stale lock /etc/.pwlock (pid %d is gone)", (int)reaped);

    if (check_refusal("an acquire took a lock naming a reaped pid", body, expected) != 0) {
        return -1;
    }
    /* The assertion the deleted age rule fails: an hour-old lock is still a
     * lock, since nothing can tell an abandoned holder from a stopped one. */
    {
        struct timespec ts[2];

        ts[0].tv_sec = time(NULL) - 3600;
        ts[0].tv_nsec = 0;
        ts[1] = ts[0];
        if (utimensat(AT_FDCWD, "/etc/.pwlock", ts, 0) != 0) {
            return fail("could not push a lock file's modification time back");
        }
    }
    if (check_refusal("an acquire took an hour-old lock", body, expected) != 0) {
        return -1;
    }

    if (unlink("/etc/.pwlock") != 0) {
        return fail("could not remove the lock by hand, which is the whole recovery");
    }
    if (cred_txn_begin("pwauthmusl") != 0) {
        return fail("an acquire failed with the lock removed");
    }
    if (!present("/etc/.pwlock")) {
        return fail("a held transaction has no lock file");
    }
    if (cred_txn_end() != 0 || present("/etc/.pwlock")) {
        return fail("ending the transaction did not drop the lock");
    }

    return 0;
}

/**
 * Read from `fd` until `buffer` has `needle` or `ms` milliseconds pass.
 * Returns 1 when the text arrived and 0 otherwise.
 *
 * Every read of the master is bounded, because a probe that blocks on a
 * terminal nobody is writing to would hang the whole suite rather than fail it.
 * A read of zero is waited through rather than taken as the end: a master whose
 * slave is not open yet reads as end of file, and the child does its own open
 * after the fork.
 */
static int expect_on(int fd, const char *needle, char *buffer, size_t size, long ms)
{
    size_t filled = strlen(buffer);
    long waited = 0;

    while (waited < ms) {
        ssize_t n = read(fd, buffer + filled, size - 1 - filled);

        if (n > 0) {
            filled += (size_t)n;
            buffer[filled] = '\0';
            if (strstr(buffer, needle) != NULL) {
                return 1;
            }
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return 0;
        }
        nap(50);
        waited += 50;
    }

    return 0;
}

/** Drain whatever `fd` has ready, returning the byte count. */
static ssize_t drain(int fd, char *buffer, size_t size)
{
    ssize_t total = 0;
    ssize_t n;

    buffer[0] = '\0';
    while ((n = read(fd, buffer + total, size - 1 - (size_t)total)) > 0) {
        total += n;
        buffer[total] = '\0';
        if ((size_t)total + 1 >= size) {
            break;
        }
    }

    return total;
}

/**
 * Wait up to `ms` milliseconds for `pid` to stop or exit, as `flags` asks.
 * Returns 1 with `*status` filled, 0 on a timeout.
 */
static int wait_for(pid_t pid, int *status, int flags, long ms)
{
    long waited = 0;

    while (waited < ms) {
        pid_t got = waitpid(pid, status, flags | WNOHANG);

        if (got == pid) {
            return 1;
        }
        if (got < 0) {
            return 0;
        }
        nap(50);
        waited += 50;
    }

    return 0;
}

/**
 * Open a pseudo-terminal and write its slave's name into `name`. Returns the
 * master, or -1.
 */
static int pty_make(char *name, size_t size)
{
    int master = open("/dev/ptmx", O_RDWR);
    int unlock = 0;
    int number = 0;

    if (master < 0) {
        return -1;
    }
    if (ioctl(master, TIOCSPTLCK, &unlock) != 0 || ioctl(master, TIOCGPTN, &number) != 0) {
        close(master);

        return -1;
    }
    snprintf(name, size, "/dev/pts/%d", number);

    return master;
}

/**
 * The prompt's side of a terminal: the child reads a password on the slave of a
 * pseudo-terminal, and this returns what it exited with.
 *
 * `setsid` and `TIOCSCTTY` are the handoff `login` and dropbear make, and they
 * are what puts the child in the foreground process group a `^Z` reaches.
 */
static void prompt_child(const char *name, int master)
{
    char password[64];
    int slave = open(name, O_RDWR);

    close(master);
    if (slave < 0) {
        _exit(41);
    }
    if (dup2(slave, 0) != 0 || dup2(slave, 1) != 1 || dup2(slave, 2) != 2) {
        _exit(42);
    }
    if (slave > 2) {
        close(slave);
    }
    if (setsid() < 0 || ioctl(0, TIOCSCTTY, 0) != 0) {
        _exit(43);
    }
    if (pwauth_read_password("Password: ", password, sizeof(password)) != 0) {
        _exit(44);
    }
    _exit(strcmp(password, "secret") == 0 ? 0 : 45);
}

/**
 * The stop and continue cycle at a password prompt, over a pseudo-terminal.
 *
 * The four things asserted are the four the terminal handling exists for: while
 * the prompt is up nothing typed is echoed; a `^Z` stops the reader with the
 * modes it found put back, so a stopped process leaves a usable terminal;
 * the continue takes echo away again, discarding what was typed meanwhile
 * rather than letting it become a password byte; and the password read across
 * the whole cycle is exactly what was typed after it.
 */
static int check_prompt(void)
{
    char buffer[512];
    char name[32];
    pid_t child;
    int master;
    int status;

    master = pty_make(name, sizeof(name));
    if (master < 0) {
        return fail("could not open and name a pseudo-terminal");
    }

    child = fork();
    if (child == 0) {
        prompt_child(name, master);
    }
    if (child < 0) {
        close(master);

        return fail("could not fork the process that reads the prompt");
    }
    fcntl(master, F_SETFL, O_NONBLOCK);

    buffer[0] = '\0';
    if (!expect_on(master, "Password: ", buffer, sizeof(buffer), 5000)) {
        close(master);

        return fail("the prompt never reached the terminal");
    }

    /* Echo is off, so what is typed at the prompt must not come back. */
    if (write(master, "abc", 3) != 3) {
        close(master);

        return fail("could not type at the prompt");
    }
    nap(500);
    if (drain(master, buffer, sizeof(buffer)) != 0) {
        close(master);

        return fail("the terminal echoed what was typed at a password prompt");
    }

    if (write(master, "\032", 1) != 1) {
        close(master);

        return fail("could not send the suspend character");
    }
    if (!wait_for(child, &status, WUNTRACED, 5000) || !WIFSTOPPED(status) ||
        WSTOPSIG(status) != SIGTSTP) {
        close(master);

        return fail("the suspend character did not stop the reader");
    }

    /* The stop handler put the modes back before the process stopped, so a
     * stopped prompt leaves a terminal that still echoes. */
    if (write(master, "xyz", 3) != 3) {
        close(master);

        return fail("could not type at the stopped reader's terminal");
    }
    buffer[0] = '\0';
    if (!expect_on(master, "xyz", buffer, sizeof(buffer), 3000)) {
        close(master);

        return fail("a stopped reader left the terminal with echo still off");
    }

    if (kill(child, SIGCONT) != 0) {
        close(master);

        return fail("could not continue the stopped reader");
    }
    buffer[0] = '\0';
    if (!expect_on(master, "Password: ", buffer, sizeof(buffer), 5000)) {
        close(master);

        return fail("the continued reader did not reprint its prompt");
    }

    if (write(master, "secret\n", 7) != 7) {
        close(master);

        return fail("could not type the password after the continue");
    }
    nap(500);
    {
        /* The reader writes one newline of its own when it puts the modes back,
         * so a blank line is expected and a character of the password is not.
         */
        ssize_t n = drain(master, buffer, sizeof(buffer));
        ssize_t i;

        for (i = 0; i < n; i++) {
            if (buffer[i] != '\n' && buffer[i] != '\r') {
                close(master);

                return fail("the continued reader echoed the password back");
            }
        }
    }
    if (!wait_for(child, &status, 0, 5000) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        close(master);

        return fail("the password read across the stop was not the one typed after it");
    }
    close(master);

    return 0;
}

/**
 * A suspend character with more bytes behind it in one write, which is what a
 * paste, an `ssh` segment, and any scripted driver deliver.
 *
 * The line discipline flushes what is queued when it recognizes the suspend
 * character, and no more: a byte that arrived behind it in the same write is
 * queued like any other and the read returns it before the stop is served. So
 * the byte is already in the reader's buffer when the continue arrives, and
 * unless the resume drops the line it is spliced onto what is typed after the
 * reprinted prompt. The reader here exits 0 only if the password it read is
 * exactly what was typed after the continue, so `X` becoming the first
 * character of the password is the failure, and it is one a user meets by
 * typing `^Z` and a shell command in the same breath.
 */
static int check_prompt_burst(void)
{
    char buffer[512];
    char name[32];
    pid_t child;
    int master;
    int status;

    master = pty_make(name, sizeof(name));
    if (master < 0) {
        return fail("could not open a pseudo-terminal for the burst case");
    }
    child = fork();
    if (child == 0) {
        prompt_child(name, master);
    }
    if (child < 0) {
        close(master);

        return fail("could not fork the reader the burst case suspends");
    }
    fcntl(master, F_SETFL, O_NONBLOCK);

    buffer[0] = '\0';
    if (!expect_on(master, "Password: ", buffer, sizeof(buffer), 5000)) {
        close(master);

        return fail("the prompt never reached the terminal in the burst case");
    }
    if (write(master, "\032X\n", 3) != 3) {
        close(master);

        return fail("could not send the suspend character and the bytes behind it");
    }
    if (!wait_for(child, &status, WUNTRACED, 5000) || !WIFSTOPPED(status) ||
        WSTOPSIG(status) != SIGTSTP) {
        close(master);

        return fail("a suspend character in a burst did not stop the reader");
    }
    if (kill(child, SIGCONT) != 0) {
        close(master);

        return fail("could not continue the reader the burst case stopped");
    }
    buffer[0] = '\0';
    if (!expect_on(master, "Password: ", buffer, sizeof(buffer), 5000)) {
        close(master);

        return fail("the continued reader did not reprint its prompt in the burst case");
    }
    if (write(master, "secret\n", 7) != 7) {
        close(master);

        return fail("could not type the password after the burst");
    }
    if (!wait_for(child, &status, 0, 5000) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        close(master);

        return fail("a byte typed behind the suspend character became a password byte");
    }
    close(master);

    return 0;
}

/**
 * The shell's side of a continue in the background: a session leader that owns
 * the terminal, a reader in a process group of its own, and a `bg`.
 *
 * It runs the whole case itself because the reader is its child and not the
 * probe's, and reports one line on `report`: the state of `ECHO` on the
 * terminal a second after the continue. `TIOCSPGRP` raises no `SIGTTOU` on this
 * kernel, so taking the terminal back from a background group needs nothing of
 * the shell dance around it.
 */
static void prompt_bg_leader(const char *name, int master, int report)
{
    struct termios tio;
    char verdict[32];
    pid_t reader;
    int slave = open(name, O_RDWR);
    int status;

    close(master);
    if (slave < 0) {
        _exit(51);
    }
    if (dup2(slave, 0) != 0 || dup2(slave, 1) != 1 || dup2(slave, 2) != 2) {
        _exit(52);
    }
    if (slave > 2) {
        close(slave);
    }
    if (setsid() < 0 || ioctl(0, TIOCSCTTY, 0) != 0) {
        _exit(53);
    }

    reader = fork();
    if (reader == 0) {
        char password[64];

        setpgid(0, 0);
        pwauth_read_password("Password: ", password, sizeof(password));
        _exit(0);
    }
    if (reader < 0) {
        _exit(54);
    }
    setpgid(reader, reader);
    if (tcsetpgrp(0, reader) != 0) {
        _exit(55);
    }
    if (waitpid(reader, &status, WUNTRACED) != reader || !WIFSTOPPED(status)) {
        _exit(56);
    }

    /* What `bg` is: the terminal goes back to the shell and only then is the
     * job continued. */
    if (tcsetpgrp(0, getpid()) != 0) {
        _exit(57);
    }
    if (kill(reader, SIGCONT) != 0) {
        _exit(58);
    }
    nap(1000);
    if (tcgetattr(0, &tio) != 0) {
        _exit(59);
    }
    snprintf(verdict, sizeof(verdict), "echo=%s\n", (tio.c_lflag & ECHO) ? "on" : "off");
    write(report, verdict, strlen(verdict));
    kill(reader, SIGKILL);
    kill(reader, SIGCONT);
    waitpid(reader, &status, 0);
    _exit(0);
}

/**
 * This image's `tcgetpgrp(3)`, and the stub that stops the reader the instant
 * it has its answer.
 *
 * The library asks who owns the terminal and then acts on the answer, and the
 * answer goes stale if a stop is served in between: a shell can reclaim the
 * terminal and continue the reader in the background, after which acting on the
 * old answer silences a terminal the reader no longer owns. That interval
 * cannot be entered from outside, so it is entered from within. It raises
 * `SIGTSTP` rather than `SIGSTOP` deliberately: `SIGTSTP` is the one a
 * keystroke sends and the one the library can defer, and a test using the
 * unblockable signal would be asserting a promise the library does not make.
 *
 * It answers from the ioctl the libc's own wrapper uses, so nothing is lost
 * by standing in for it.
 */
static volatile sig_atomic_t Tcg_stop;

pid_t tcgetpgrp(int fd)
{
    int group = 0;

    if (ioctl(fd, TIOCGPGRP, &group) != 0) {
        return -1;
    }
    if (Tcg_stop) {
        Tcg_stop = 0;
        raise(SIGTSTP);
    }

    return (pid_t)group;
}

/**
 * The session leader for the pre-read stop case: it keeps the reader in the
 * foreground throughout and only continues it, so the password is typed to a
 * reader that owns the terminal.
 */
static void prompt_resume_leader(const char *name, int master, int report)
{
    char verdict[32];
    pid_t reader;
    int slave = open(name, O_RDWR);
    int status;

    close(master);
    if (slave < 0) {
        _exit(51);
    }
    if (dup2(slave, 0) != 0 || dup2(slave, 1) != 1 || dup2(slave, 2) != 2) {
        _exit(52);
    }
    if (slave > 2) {
        close(slave);
    }
    if (setsid() < 0 || ioctl(0, TIOCSCTTY, 0) != 0) {
        _exit(53);
    }

    reader = fork();
    if (reader == 0) {
        char password[64];

        setpgid(0, 0);
        Tcg_stop = 1;
        if (pwauth_read_password("Password: ", password, sizeof(password)) != 0) {
            _exit(61);
        }
        _exit(strcmp(password, "secret") == 0 ? 0 : 62);
    }
    if (reader < 0) {
        _exit(54);
    }
    setpgid(reader, reader);
    if (tcsetpgrp(0, reader) != 0) {
        _exit(55);
    }
    if (waitpid(reader, &status, WUNTRACED) != reader || !WIFSTOPPED(status)) {
        _exit(56);
    }
    /* The terminal stays the reader's: this is a `fg`, not a `bg`. */
    kill(reader, SIGCONT);
    verdict[0] = 'r';
    (void)write(report, verdict, 1);
    if (waitpid(reader, &status, 0) != reader) {
        _exit(58);
    }
    verdict[0] = (char)(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? '1' : '0');
    (void)write(report, verdict, 1);
    _exit(0);
}

/**
 * A stop served before the first read, and the password typed after the resume.
 *
 * The reader drops its line whenever a resume is seen, because a byte typed
 * behind the suspend character can be returned by a read before the stop is
 * served and must not become a password byte. But the stop can also land while
 * the modes are being set or the prompt written, with no read outstanding at
 * all, and then the first read after the resume returns the first character the
 * user genuinely typed. Dropping that one turns a correct password into a
 * refusal, which is a fault the user cannot see the cause of: they type the
 * right thing and are told it is wrong.
 *
 * The stub stops the reader at exactly that point. Unlike the stale-owner case
 * the terminal stays with the reader, so what follows is an ordinary `fg` and
 * an ordinary password, and every character of it must survive.
 */
static int check_prompt_stop_before_read(void)
{
    char verdict[8];
    pid_t leader;
    int master;
    int channel[2];
    int status;
    char name[32];

    master = pty_make(name, sizeof(name));
    if (master < 0) {
        return fail("could not open a pseudo-terminal for the pre-read stop case");
    }
    if (pipe(channel) != 0) {
        close(master);

        return fail("could not make the pipe the pre-read stop case reports on");
    }
    leader = fork();
    if (leader == 0) {
        close(channel[0]);
        prompt_resume_leader(name, master, channel[1]);
    }
    close(channel[1]);
    if (leader < 0) {
        close(master);
        close(channel[0]);

        return fail("could not fork the session leader the pre-read stop case needs");
    }
    if (read(channel[0], verdict, 1) != 1 || verdict[0] != 'r') {
        close(master);
        close(channel[0]);

        return fail("the pre-read stop case never saw its reader resume");
    }
    if (write(master, "secret\n", 7) != 7) {
        close(master);
        close(channel[0]);

        return fail("could not type the password the pre-read stop case checks");
    }
    if (read(channel[0], verdict, 1) != 1) {
        close(master);
        close(channel[0]);

        return fail("the pre-read stop case never heard how its reader finished");
    }
    close(channel[0]);
    close(master);
    if (waitpid(leader, &status, 0) != leader) {
        return fail("could not reap the session leader of the pre-read stop case");
    }
    if (verdict[0] != '1') {
        return fail("a password typed after a stop served before the first read lost a character");
    }

    return 0;
}

/**
 * The session leader for the stop-during-acquisition case: it hands the reader
 * the terminal, waits for the stub to stop it, takes the terminal back, and
 * continues it in the background.
 */
static void prompt_stale_leader(const char *name, int master, int report)
{
    struct termios tio;
    char verdict[32];
    pid_t reader;
    int slave = open(name, O_RDWR);
    int status;

    close(master);
    if (slave < 0) {
        _exit(51);
    }
    if (dup2(slave, 0) != 0 || dup2(slave, 1) != 1 || dup2(slave, 2) != 2) {
        _exit(52);
    }
    if (slave > 2) {
        close(slave);
    }
    if (setsid() < 0 || ioctl(0, TIOCSCTTY, 0) != 0) {
        _exit(53);
    }

    reader = fork();
    if (reader == 0) {
        char password[64];

        setpgid(0, 0);
        Tcg_stop = 1;
        pwauth_read_password("Password: ", password, sizeof(password));
        _exit(0);
    }
    if (reader < 0) {
        _exit(54);
    }
    setpgid(reader, reader);
    if (tcsetpgrp(0, reader) != 0) {
        _exit(55);
    }
    /* It stops inside the library, one call after it asked who owns this. */
    if (waitpid(reader, &status, WUNTRACED) != reader || !WIFSTOPPED(status)) {
        _exit(56);
    }
    /* The shell takes its terminal back and puts the job in the background. */
    if (tcsetpgrp(0, getpgrp()) != 0) {
        _exit(57);
    }
    kill(reader, SIGCONT);
    if (waitpid(reader, &status, WUNTRACED) != reader) {
        _exit(58);
    }
    verdict[0] = (char)(tcgetattr(0, &tio) == 0 && (tio.c_lflag & ECHO) != 0 ? '1' : '0');
    verdict[1] = (char)(WIFSTOPPED(status) ? '1' : '0');
    (void)write(report, verdict, 2);
    kill(reader, SIGKILL);
    (void)kill(reader, SIGCONT);
    (void)waitpid(reader, &status, 0);
    _exit(0);
}

/**
 * A reader stopped between asking who owns the terminal and acting on it.
 *
 * The window the ownership check opens if the two halves are not one step. The
 * reader is the foreground group when it asks, is stopped before it can act,
 * and is continued in the background by a leader that has taken the terminal
 * back. Acting on the stale answer silences a terminal belonging to somebody
 * else, which is the same damage as the background case and reachable the same
 * way, by one `^Z` at a password prompt.
 */
static int check_prompt_stale_owner(void)
{
    char verdict[8];
    pid_t leader;
    int master;
    int channel[2];
    int status;
    ssize_t got;
    char name[32];

    master = pty_make(name, sizeof(name));
    if (master < 0) {
        return fail("could not open a pseudo-terminal for the stale-owner case");
    }
    if (pipe(channel) != 0) {
        close(master);

        return fail("could not make the pipe the stale-owner case reports on");
    }
    leader = fork();
    if (leader == 0) {
        close(channel[0]);
        prompt_stale_leader(name, master, channel[1]);
    }
    close(channel[1]);
    if (leader < 0) {
        close(master);
        close(channel[0]);

        return fail("could not fork the session leader the stale-owner case needs");
    }
    got = read(channel[0], verdict, 2);
    close(channel[0]);
    close(master);
    if (waitpid(leader, &status, 0) != leader) {
        return fail("could not reap the session leader of the stale-owner case");
    }
    if (got != 2) {
        return fail("the stale-owner case never heard from its session leader");
    }
    if (verdict[0] != '1') {
        return fail("a reader continued in the background acted on a stale ownership answer");
    }

    return 0;
}

/**
 * The child for the overflow-then-resume case: one read, which must come back
 * with the line typed AFTER the stop.
 */
static void prompt_overflow_child(int in)
{
    char password[64];

    if (dup2(in, 0) != 0) {
        _exit(42);
    }
    if (in > 2) {
        close(in);
    }
    if (pwauth_read_password("Password: ", password, sizeof(password)) != 0) {
        _exit(44);
    }
    _exit(strcmp(password, "secret") == 0 ? 0 : 45);
}

/**
 * A run too long to keep, then a stop, then a line that fits.
 *
 * The reader refuses an over-long entry rather than storing the prefix it
 * fitted, and it drops the line it has whenever a resume is served. Those two
 * meet here: a resume starts a fresh line, so the overflow that ended the old
 * one must go with it. Left standing, the flag rejects the perfectly good
 * password typed after the continue, and the user is told their entry was too
 * long when it was not, with no way past it but to start the program again.
 *
 * The channel is a pipe and not a pseudo-terminal, which is the whole reason
 * this case can exist. A terminal here is canonical, so nothing reaches the
 * reader until a newline does and an over-long line therefore ends the read
 * before any stop can be served; on a pipe the bytes arrive as they are
 * written, so the reader can be holding an over-long run when the stop lands.
 * Driving a program by script, which is how these probes and dropbear both run
 * one, is exactly that case.
 */
static int check_prompt_overflow_resume(void)
{
    char filler[200];
    int in[2];
    pid_t child;
    int status;
    size_t i;

    if (pipe(in) != 0) {
        return fail("could not make the pipe the overflow-resume case feeds");
    }
    child = fork();
    if (child == 0) {
        close(in[1]);
        prompt_overflow_child(in[0]);
    }
    close(in[0]);
    if (child < 0) {
        close(in[1]);

        return fail("could not fork the reader the overflow-resume case interrupts");
    }

    /* More than the reader will keep, and no newline, so it is still collecting
     * when the stop arrives. */
    for (i = 0; i < sizeof(filler) - 1; i++) {
        filler[i] = 'a';
    }
    filler[sizeof(filler) - 1] = '\0';
    if (write(in[1], filler, sizeof(filler) - 1) != (ssize_t)(sizeof(filler) - 1)) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        close(in[1]);

        return fail("could not write the over-long run");
    }
    sleep(1);
    kill(child, SIGTSTP);
    if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status)) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        close(in[1]);

        return fail("the reader did not stop in the overflow-resume case");
    }
    kill(child, SIGCONT);
    sleep(1);
    if (write(in[1], "secret\n", 7) != 7) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        close(in[1]);

        return fail("could not write the password after the resume");
    }
    close(in[1]);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return fail("the reader did not finish the overflow-resume case");
    }
    if (WEXITSTATUS(status) == 44) {
        return fail("a line that fits was refused because an earlier one had not");
    }
    if (WEXITSTATUS(status) != 0) {
        return fail("the password typed after the resume was not the one read");
    }

    return 0;
}

/**
 * The session leader for the never-foreground case: it keeps the terminal and
 * never hands it over, which is the shape a shell's `&` leaves behind.
 */
static void prompt_unowned_leader(const char *name, int master, int report)
{
    struct termios tio;
    char verdict[32];
    pid_t reader;
    int slave = open(name, O_RDWR);
    int status;

    close(master);
    if (slave < 0) {
        _exit(51);
    }
    if (dup2(slave, 0) != 0 || dup2(slave, 1) != 1 || dup2(slave, 2) != 2) {
        _exit(52);
    }
    if (slave > 2) {
        close(slave);
    }
    if (setsid() < 0 || ioctl(0, TIOCSCTTY, 0) != 0) {
        _exit(53);
    }

    reader = fork();
    if (reader == 0) {
        char password[64];

        setpgid(0, 0);
        pwauth_read_password("Password: ", password, sizeof(password));
        _exit(0);
    }
    if (reader < 0) {
        _exit(54);
    }
    setpgid(reader, reader);
    /* No tcsetpgrp: the terminal stays this process's, so the reader is a
     * background group throughout, which is the whole point. */
    if (waitpid(reader, &status, WUNTRACED) != reader) {
        _exit(56);
    }
    verdict[0] = (char)(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN ? '1' : '0');
    verdict[1] = (char)(tcgetattr(0, &tio) == 0 && (tio.c_lflag & ECHO) != 0 ? '1' : '0');
    (void)write(report, verdict, 2);
    kill(reader, SIGKILL);
    (void)waitpid(reader, &status, 0);
    _exit(0);
}

/**
 * A reader that never owns the terminal must not take echo from it.
 *
 * `su &` is one keystroke, and this kernel raises nothing for a `tcsetattr`
 * from a background process group: only the read is guarded, by `SIGTTIN`. A
 * reader that silenced the terminal before reading would leave the shell that
 * started it echoing nothing, with no program running that knows how to put it
 * back, and killing the stopped job would not undo it because the modes belong
 * to the terminal.
 *
 * The host test cannot ask this question, since Darwin stops a background
 * `tcsetattr` with `SIGTTOU` before it takes effect and this kernel does not.
 * So it is asked here, where the permission is real. Both halves are asserted:
 * the reader stopped on `SIGTTIN` rather than being handed a byte, and the
 * terminal still echoes.
 */
static int check_prompt_never_owned(void)
{
    char verdict[8];
    pid_t leader;
    int master;
    int channel[2];
    int status;
    ssize_t got;
    char name[32];

    master = pty_make(name, sizeof(name));
    if (master < 0) {
        return fail("could not open a pseudo-terminal for the never-foreground case");
    }
    if (pipe(channel) != 0) {
        close(master);

        return fail("could not make the pipe the never-foreground case reports on");
    }
    leader = fork();
    if (leader == 0) {
        close(channel[0]);
        prompt_unowned_leader(name, master, channel[1]);
    }
    close(channel[1]);
    if (leader < 0) {
        close(master);
        close(channel[0]);

        return fail("could not fork the session leader the never-foreground case needs");
    }
    got = read(channel[0], verdict, 2);
    close(channel[0]);
    close(master);
    if (waitpid(leader, &status, 0) != leader) {
        return fail("could not reap the session leader of the never-foreground case");
    }
    if (got != 2) {
        return fail("the never-foreground case never heard from its session leader");
    }
    if (verdict[0] != '1') {
        return fail("a background reader did not stop on SIGTTIN as it should have");
    }
    if (verdict[1] != '1') {
        return fail("a background reader took echo from a terminal it did not own");
    }

    return 0;
}

/**
 * A reader continued in the background, which is a shell's `bg` and nothing
 * exotic.
 *
 * A continue does not carry the terminal with it. A reader that re-applies
 * echo-off and reprints its prompt on every `SIGCONT` therefore silences a
 * terminal that now belongs to the shell and prints into the shell's session,
 * and no `SIGTTOU` stops it, since this kernel raises none for `TCSETS`. So the
 * two assertions are what the shell must still have afterwards: its prompt not
 * overwritten, and a terminal that still echoes.
 */
static int check_prompt_background(void)
{
    char buffer[512];
    char verdict[64];
    char name[32];
    pid_t leader;
    int master;
    int channel[2];
    int status;

    master = pty_make(name, sizeof(name));
    if (master < 0) {
        return fail("could not open a pseudo-terminal for the background case");
    }
    if (pipe(channel) != 0) {
        close(master);

        return fail("could not make the pipe the background case reports on");
    }
    leader = fork();
    if (leader == 0) {
        close(channel[0]);
        prompt_bg_leader(name, master, channel[1]);
    }
    close(channel[1]);
    if (leader < 0) {
        close(master);
        close(channel[0]);

        return fail("could not fork the session leader the background case needs");
    }
    fcntl(master, F_SETFL, O_NONBLOCK);
    fcntl(channel[0], F_SETFL, O_NONBLOCK);

    buffer[0] = '\0';
    if (!expect_on(master, "Password: ", buffer, sizeof(buffer), 5000)) {
        close(master);
        close(channel[0]);

        return fail("the prompt never reached the terminal in the background case");
    }
    if (write(master, "\032", 1) != 1) {
        close(master);
        close(channel[0]);

        return fail("could not send the suspend character in the background case");
    }

    verdict[0] = '\0';
    if (!expect_on(channel[0], "echo=", verdict, sizeof(verdict), 10000)) {
        close(master);
        close(channel[0]);

        return fail("the session leader did not report what the continue left behind");
    }
    close(channel[0]);
    buffer[0] = '\0';
    drain(master, buffer, sizeof(buffer));
    close(master);
    if (waitpid(leader, &status, 0) != leader || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the session leader could not drive the background case");
    }
    if (strstr(buffer, "Password: ") != NULL) {
        return fail("a reader continued in the background reprinted its prompt");
    }
    if (strstr(verdict, "echo=on") == NULL) {
        return fail("a reader continued in the background took echo from the shell");
    }

    return 0;
}

/**
 * A standard error that is a pipe with no reader, at a prompt and under a
 * transaction.
 *
 * This kernel raises `SIGPIPE` on a write to a pipe every reader has closed,
 * and
 * its default action ends the process. The prompt is written to file descriptor
 * 2 after echo has been taken away, so that death leaves the console silenced
 * with no program left to restore it; and a writer's diagnostic reaches the
 * same
 * write with the lock held, where the death runs no `atexit` and the lock file
 * stays in `/etc`. So the cases are the two lifetimes, and each is a child,
 * because what is being guarded against is a death rather than a return value.
 * The writer's child leaves through `exit`, so the release must have run: a
 * lock
 * still at the name afterwards is exactly the stranded lock this is about.
 */
static int check_sigpipe(void)
{
    char password[64];
    int broken[2];
    int feed[2];
    pid_t child;
    int status;

    unlink("/etc/.pwlock");
    if (pipe(broken) != 0 || pipe(feed) != 0) {
        return fail("could not make the pipes the SIGPIPE cases need");
    }
    if (write(feed[1], "secret\n", 7) != 7) {
        return fail("could not script the line the prompt case reads");
    }
    close(feed[1]);
    close(broken[0]); /* from here the write end has no reader anywhere */

    child = fork();
    if (child == 0) {
        if (dup2(feed[0], 0) != 0 || dup2(broken[1], 2) != 2) {
            _exit(61);
        }
        if (pwauth_read_password("Password: ", password, sizeof(password)) != 0) {
            _exit(62);
        }
        _exit(strcmp(password, "secret") == 0 ? 0 : 63);
    }
    close(feed[0]);
    if (child < 0 || waitpid(child, &status, 0) != child) {
        close(broken[1]);

        return fail("could not run the prompt whose standard error nobody reads");
    }
    if (!WIFEXITED(status)) {
        close(broken[1]);

        return fail("a prompt written into a pipe with no reader killed the reader");
    }
    if (WEXITSTATUS(status) != 0) {
        close(broken[1]);

        return fail("a prompt written into a pipe with no reader did not read its line");
    }

    child = fork();
    if (child == 0) {
        if (cred_txn_begin("pwauthmusl") != 0) {
            _exit(64);
        }
        if (dup2(broken[1], 2) != 2) {
            child_exit(65);
        }
        cred_rewrite(NULL, NULL, 0); /* one line, into a pipe with no reader */
        exit(0);                     /* the release registered with atexit must still run */
    }
    close(broken[1]);
    if (child < 0 || waitpid(child, &status, 0) != child) {
        return fail("could not run the writer whose standard error nobody reads");
    }
    if (!WIFEXITED(status)) {
        unlink("/etc/.pwlock");

        return fail("a writer reporting into a pipe with no reader was killed holding its lock");
    }
    if (WEXITSTATUS(status) != 0) {
        unlink("/etc/.pwlock");

        return fail("the writer whose standard error nobody reads could not take its transaction");
    }
    if (present("/etc/.pwlock")) {
        unlink("/etc/.pwlock");

        return fail("a writer killed at its own diagnostic left its lock behind");
    }

    return 0;
}

/**
 * The caller `su` is entered from: a uid-1000 process that hands `password` to
 * `/su` on a pipe and reports what the program su execs exited with.
 *
 * It is a process of its own for two reasons. `su` decides from the real uid,
 * so it has to be entered from a real uid-1000 caller; and a refused `su` must
 * leave that caller exactly as it was, which only the caller can say. It exits
 * with the run's status, or with its own code when the run died of a signal or
 * left the caller's identity changed.
 */
static void su_caller(const char *cwd, char *const argv[], char *const envp[], const char *password)
{
    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;
    int feed[2];
    pid_t runner;
    int status;

    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        _exit(CALLER_BROKEN);
    }
    if (chdir(cwd) != 0 || pipe(feed) != 0) {
        _exit(CALLER_BROKEN);
    }
    runner = fork();
    if (runner == 0) {
        close(feed[1]);
        if (dup2(feed[0], 0) != 0) {
            _exit(CALLER_BROKEN);
        }
        if (feed[0] > 0) {
            close(feed[0]);
        }
        execve("/su", argv, envp);
        _exit(CALLER_BROKEN);
    }
    close(feed[0]);
    if (runner < 0) {
        _exit(CALLER_BROKEN);
    }
    dprintf(feed[1], "%s\n", password);
    close(feed[1]);

    if (waitpid(runner, &status, 0) != runner || !WIFEXITED(status)) {
        _exit(CALLER_SIGNAL);
    }
    if (getresuid(&ruid, &euid, &suid) != 0 || getresgid(&rgid, &egid, &sgid) != 0 ||
        ruid != 1000 || euid != 1000 || suid != 1000 || rgid != 1000 || egid != 1000 ||
        sgid != 1000) {
        _exit(CALLER_BROKEN);
    }
    _exit(WEXITSTATUS(status) & 0x7f);
}

/**
 * Run one `su` case and return the status the program su execs left, or -1 if
 * the caller itself could not be run.
 */
static int run_su(const char *cwd, char *const argv[], char *const envp[], const char *password)
{
    pid_t caller = fork();
    int status;

    if (caller == 0) {
        su_caller(cwd, argv, envp, password);
    }
    if (caller < 0 || waitpid(caller, &status, 0) != caller || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

/**
 * The signals above the host's reach, and the fault-numbered ones, which only
 * the guest can speak for.
 *
 * The library derives the set it handles from what this kernel does, which
 * terminates a process for every signal but a few, over the whole range 1..64.
 * The host test checks that derivation against the dispositions actually
 * installed, but Darwin stops at 32, so the upper half is unchecked there and
 * is checked here instead. Two claims are made about it. A signal in the upper
 * range must be handled like any other death: the writer leaves through `exit`
 * and its lock is gone. And 32, 33 and 34 must NOT be handled, because this
 * libc reserves them and refuses them from `sigaction`, which is a residue the
 * library names rather than one it hides: a writer sent one of those dies of it
 * with the lock behind, exactly as `SIGKILL` leaves one.
 *
 * The fault numbers are here for a different reason. A real CPU exception on
 * this kernel never reaches a disposition: the exception handler and the
 * page fault path both call `terminate_current_process`, which zombifies the
 * process without consulting `sigacts`. So a `SIGSEGV` a handler can see is
 * one that was sent, which is an ordinary death, and the library handles it
 * like one. Sending them here is what proves that, since nothing in a host
 * test can distinguish a signal this kernel delivers from one it does not.
 */
static int check_high_signals(void)
{
    static const int handled[] = {35, 50, 64, SIGSEGV, SIGILL, SIGFPE};
    static const int reserved[] = {32, 33, 34};
    size_t i;

    for (i = 0; i < sizeof(handled) / sizeof(handled[0]); i++) {
        pid_t child;
        int status;

        unlink("/etc/.pwlock");
        child = fork();
        if (child == 0) {
            if (cred_txn_begin("pwauthmusl") != 0) {
                _exit(70);
            }
            for (;;) {
                sleep(1);
            }
        }
        if (child < 0) {
            return fail("could not fork the writer the high-signal case signals");
        }
        while (!present("/etc/.pwlock")) {
            sleep(1);
        }
        kill(child, handled[i]);
        if (waitpid(child, &status, 0) != child) {
            return fail("could not reap the writer the high-signal case signals");
        }
        if (WIFSIGNALED(status)) {
            return fail("a writer sent a signal above 34 took the default action");
        }
        if (!WIFEXITED(status)) {
            return fail("a writer sent a signal above 34 ended in no known way");
        }
        if (present("/etc/.pwlock")) {
            unlink("/etc/.pwlock");

            return fail("a writer sent a signal above 34 left its lock behind");
        }
    }

    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        struct sigaction probe;

        memset(&probe, 0, sizeof(probe));
        probe.sa_handler = SIG_IGN;
        sigemptyset(&probe.sa_mask);
        probe.sa_flags = 0;
        if (sigaction(reserved[i], &probe, NULL) == 0) {
            return fail("this libc accepted a signal the library excluded as unreachable");
        }
    }

    return 0;
}

/**
 * `su` itself: the non-login run, a refused one, and the login run.
 *
 * The two successful runs differ in one argument and in nothing else, so what
 * the login run proves (a working directory of the target's home and a
 * leading-dash `argv[0]`) is exactly what the non-login run proves is not done.
 * The caller's own variable is in the environment of both, and reaches neither.
 */
static int check_su(void)
{
    char *caller_env[] = {(char *)"SU_LEAK=1", (char *)"PATH=/nowhere", NULL};
    char *non_login[] = {(char *)"su", (char *)"probe", (char *)"-c", (char *)EXPECT_NON_LOGIN,
                         NULL};
    char *login[] = {(char *)"su", (char *)"-",          (char *)"probe",
                     (char *)"-c", (char *)EXPECT_LOGIN, NULL};
    int status;

    status = run_su("/musl", non_login, caller_env, PROBE_PASSWORD);
    if (status == CALLER_BROKEN || status < 0) {
        return fail("could not run su from a uid-1000 caller");
    }
    if (status != 0) {
        printf("pwauthmusl: su without a dash left status %d\n", status);

        return fail("a non-login su did not run the target's shell as it was");
    }

    /* The limit belongs to the library, not to whichever buffer a caller
     * happened to declare. `su` asks with a buffer of its own size, and an
     * entry longer than the shortest limit any reader imposes must still be
     * refused here: a password `su` accepted and dropbear refuses is an account
     * that works at the console and never over the network. */
    {
        char overlong[160];
        char saved[4096];
        char rebuilt[4096];
        char *made;
        size_t i;

        for (i = 0; i < sizeof(overlong) - 1; i++) {
            overlong[i] = 'a';
        }
        overlong[sizeof(overlong) - 1] = '\0';

        /* The account's real password is made this long, which `passwd` would
         * refuse to do and only a hand-written record can arrange. Feeding a
         * long WRONG password would prove nothing: it is refused for being
         * wrong. Feeding the long RIGHT one asks the only question there is,
         * which is whether the reader stops at the limit every other reader in
         * the image stops at. */
        if (slurp("/etc/shadow", saved, sizeof(saved)) < 0) {
            return fail("cannot read the shadow file the long-password case rewrites");
        }
        made = crypt(overlong, "$6$sulimit");
        if (!made || made[0] != '$') {
            return fail("cannot mint the hash the long-password case needs");
        }
        if (replace_field(saved, "probe", made, rebuilt, sizeof(rebuilt)) != 0 ||
            spill("/etc/shadow", rebuilt, 0600) != 0) {
            (void)spill("/etc/shadow", saved, 0600);

            return fail("could not stage the account with a long password");
        }
        status = run_su("/musl", non_login, caller_env, overlong);
        (void)spill("/etc/shadow", saved, 0600);
        if (status == 0) {
            return fail("su accepted a password longer than any reader in the image will take");
        }
    }

    status = run_su("/musl", non_login, caller_env, ROOT_PASSWORD);
    if (status == CALLER_BROKEN) {
        return fail("a refused su left the caller's identity changed");
    }
    if (status < 0 || status == 0) {
        return fail("su accepted another account's password");
    }

    status = run_su("/musl", login, caller_env, PROBE_PASSWORD);
    if (status == CALLER_BROKEN || status < 0) {
        return fail("could not run a login su from a uid-1000 caller");
    }
    if (status != 0) {
        printf("pwauthmusl: su with a dash left status %d\n", status);

        return fail("a login su did not enter the target's home under a dashed argv[0]");
    }

    return 0;
}

/**
 * Assert that nothing was left behind, put the credential databases back if
 * anything moved them, and remove this probe's own fixtures.
 *
 * The absence check runs before the removal, since removing a lock this probe
 * left would be the library's rule broken by its own test.
 */
static int finish(void)
{
    char buffer[512];
    size_t i;
    int left = 0;

    if (present("/etc/.pwlock")) {
        printf("pwauthmusl: /etc/.pwlock is still there\n");
        left = 1;
    }
    for (i = 0; i < sizeof(Replaced) / sizeof(Replaced[0]); i++) {
        if (temps_left(Replaced[i]) != 0) {
            printf("pwauthmusl: a temporary file of %s is still there\n", Replaced[i]);
            left = 1;
        }
    }
    for (i = 0; i < sizeof(Fixtures) / sizeof(Fixtures[0]); i++) {
        unlink(Fixtures[i]);
    }
    for (i = 0; i < 3; i++) {
        ssize_t n = slurp(Databases[i], buffer, sizeof(buffer));

        if (n == Snapshot_len[i] && n >= 0 && memcmp(buffer, Snapshot[i], (size_t)n) == 0) {
            continue;
        }
        if (Snapshot_len[i] < 0) {
            continue;
        }
        printf("pwauthmusl: putting %s back\n", Databases[i]);
        spill(Databases[i], Snapshot[i], i == 1 ? 0600 : 0644);
    }
    if (left) {
        return fail("a lock or a temporary file outlived the run");
    }

    return 0;
}

int main(void)
{
    size_t i;
    int rc;

    for (i = 0; i < 3; i++) {
        Snapshot_len[i] = slurp(Databases[i], Snapshot[i], sizeof(Snapshot[i]));
        if (Snapshot_len[i] < 0) {
            printf("PWAUTHMUSL FAIL (cannot read %s)\n", Databases[i]);

            return 1;
        }
    }

    rc = check_salt();
    if (rc == 0) {
        rc = check_rewrite();
    }
    if (rc == 0) {
        rc = check_fork();
    }
    if (rc == 0) {
        rc = check_lost_lock();
    }
    if (rc == 0) {
        rc = check_interleave();
    }
    if (rc == 0) {
        rc = check_recovery();
    }
    if (rc == 0) {
        rc = check_prompt();
    }
    if (rc == 0) {
        rc = check_prompt_burst();
    }
    if (rc == 0) {
        rc = check_prompt_overflow_resume();
    }
    if (rc == 0) {
        rc = check_prompt_stop_before_read();
    }
    if (rc == 0) {
        rc = check_prompt_stale_owner();
    }
    if (rc == 0) {
        rc = check_prompt_never_owned();
    }
    if (rc == 0) {
        rc = check_prompt_background();
    }
    if (rc == 0) {
        rc = check_sigpipe();
    }
    if (rc == 0) {
        rc = check_high_signals();
    }
    if (rc == 0) {
        rc = check_su();
    }
    if (finish() != 0) {
        rc = -1;
    }
    if (rc != 0) {
        return 1;
    }

    printf("PWAUTHMUSL OK\n");

    return 0;
}
