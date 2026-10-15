/**
 * pwauthmusl: check the credential library and `su` in the guest, through the
 * ported musl.
 *
 * link-deps: utils/pwauth.c
 *
 * The host test covers what is pure. This probe covers what needs the kernel:
 * the guest's `/etc`, two processes racing for one transaction, a terminal,
 * and the setuid-root `su` staged beside it. Its cases, in the order they run:
 *
 * The salt draw, first, as the control the no-entropy run inverts: on this
 * boot's `-cpu max` the mint must succeed, produce `PWAUTH_SALT_LEN`
 * characters of the `crypt(3)` alphabet, and produce a different salt the
 * second time.
 *
 * The replacement: the file is replaced whole, its mode and owner are the
 * original's (root and uid 1000 differ here, so the inheritance is tested),
 * no temporary file is left behind, and a reader holding a descriptor across
 * the rename goes on reading the file it opened, whole. The rule that readers
 * take no lock rests on that last one. A second row has a set-id bit: the
 * owner must be set before the mode because a chown clears those bits, and
 * only here can a file have both a set-id bit and an owner the writer is not.
 *
 * A fork under the transaction, since the state is file-scope and a child
 * inherits it while holding nothing: a child that exits, a child a signal
 * kills, and a child that calls the three entry points must all leave the
 * parent's lock where it is. Then a lock cleared out from under a live holder,
 * the same rule turned on the library: with another writer's lock at the name,
 * neither the publish nor the release may touch it, and the recorded pid is
 * the only thing that can tell the two files apart, since `st_ino` on the boot
 * root is derived from a hash of the name.
 *
 * The interleave, which no `O_EXCL` on a temporary file can pass: two children
 * read and rewrite one file, ordered by a pipe so the second begins its
 * transaction while the first is asleep inside its own. Both lines must
 * survive. Without the lock the second reads the pre-image and its rename
 * destroys the first one's line, the lost update the transaction prevents; the
 * second's acquire is also timed, since a lock that never blocked could pass on
 * ordering luck alone.
 *
 * The recovery rules, against `/etc/.pwlock` and the compiled-in retry budget,
 * each one an assertion that nothing was broken: a lock naming a reaped child's
 * pid is refused with the file still there and its bytes unchanged, the same
 * lock with its modification time pushed back an hour is refused as the fresh
 * one was, and the probe's own removal of the file lets the next acquire
 * succeed. Waiting out that budget twice is most of this probe's running time;
 * the bound itself is checked on the host, where the budget is a parameter.
 *
 * The stop and continue cycle, which needs a terminal and so runs over a
 * pseudo-terminal, not this probe's own console: a child in the pty's
 * foreground group reads a password, and the parent drives the master. Nothing
 * typed comes back while the prompt is up, a `^Z` stops the child with the
 * terminal's modes restored (what is typed while it is stopped is echoed), a
 * continue re-applies echo-off and reprints the prompt, and the child exits 0
 * only if the password it read is what was typed after the continue:
 * everything typed before it was flushed, and none of it became a password
 * byte.
 *
 * Then the cases a single keystroke does not cover. A suspend character with
 * bytes behind it in the same write, which the discipline does not flush and
 * the read returns before the stop is served. An over-long run on a pipe, cut
 * by a stop, then a line that fits. A stop served before the first read, and
 * one served between asking who owns the terminal and acting on the answer. A
 * reader that never owns the terminal. And a continue that arrives without the
 * terminal, which is a shell's `bg`, where re-applying echo-off and reprinting
 * would silence a terminal that belongs to the shell.
 *
 * Then a standard error that is a pipe with no reader, which this kernel
 * answers with a `SIGPIPE` whose default action is death: at the prompt that
 * death would leave the console with echo off, and under a transaction it
 * would leave the lock file in `/etc`, since no `atexit` runs on a signal.
 *
 * Then the signals the host cannot send: those above 32, which must be handled
 * as deaths, the three musl reserves, which cannot be, and the fault signals,
 * sent with `kill`.
 *
 * Then `su` itself, from a uid-1000 caller with the password on its standard
 * input: the non-login run, whose expectations are the paired half that makes
 * the login run mean something; the account's own password made longer than
 * `PWAUTH_MAX_PASSWORD`, refused; a wrong password refused with the caller's
 * identity unchanged; and the login run, which must land in the target's home
 * under a leading-dash `argv[0]`. The runs are made from a working directory of
 * `/musl` with a variable of the caller's own in the environment, so the login
 * run shows the working directory changed and the non-login run shows it did
 * not, and neither may carry the caller's variable through.
 *
 * It snapshots `/etc/passwd`, `/etc/shadow` and `/etc/group` at entry and puts
 * them back byte for byte, removes its own fixtures, and asserts before it does
 * that no lock and no temporary file is left anywhere it touched.
 *
 * Prints "PWAUTHMUSL OK" on success; any failure prints
 * "PWAUTHMUSL FAIL (detail)" and exits 1.
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

/* The exit statuses of a uid-1000 caller that came back from `su` with its
 * identity changed, and of one whose `su` died of a signal. Both are failures
 * of the run itself, and are kept clear of the small statuses su and idreport
 * use. */
#define CALLER_BROKEN 127
#define CALLER_SIGNAL 126

/* The files this probe creates, which `finish` removes. */
static const char *Fixtures[] = {"/credtxn.txt", "/credbusy.txt", "/credmode.txt", "/credlog.txt",
                                 "/credlost.txt"};

/* The files replaced under a transaction, each swept for a leftover temporary
 * file, which outlives a replacement only after a crash. A replacement writes
 * through `<path>.tmp.<pid>`, so each entry is a prefix to sweep for: a
 * temporary left by one of this probe's children has that child's pid. The
 * lock is checked beside them, since a lock outlives its holder only after a
 * kill. */
static const char *Replaced[] = {"/etc/passwd",  "/etc/shadow",   "/etc/group",
                                 "/credtxn.txt", "/credmode.txt", "/credlost.txt"};

/* The credential databases and their bytes at entry, which `finish` puts
 * back. */
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
 * Count the temporary files of `path` left in the directory holding it.
 *
 * A replacement writes through `<path>.tmp.<pid>`, and a temporary left by one
 * of this probe's children is named for that child, so this counts every name
 * that begins with the base name of `path` and `.tmp`.
 *
 * `path` must be absolute. A relative one counts as one temporary, as does a
 * directory that cannot be opened, so that a failure to look fails the probe
 * instead of passing it.
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

/** Sleep `ms` milliseconds. */
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
 * The rule that readers take no lock rests on the reader: holding a descriptor
 * across the rename, it must read the old file entire, not a prefix of it and
 * not a mixture of the two.
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
 * A `fork` under a held transaction, at `/etc/.pwlock`, with signals sent.
 *
 * The state is file-scope, so a child inherits it while holding nothing: the
 * lock names its parent. A child reaches the release by exiting or by a death
 * signal, and one keystroke sends one, since `^C` at a terminal goes to every
 * process in the foreground group. So a child that exits and a child that a
 * signal kills must leave the lock where it is, and the three entry points must
 * all decline to act on a transaction that is not the child's. The child's own
 * acquire is the `_at` form with no retries, so that it spends no time waiting:
 * this case checks which refusal it gets, and `check_recovery` waits out the
 * budget.
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
 * This is the second reason `pwauth.h` gives for never breaking a lock, turned
 * on the library itself. Root removing a lock the library reported as stale,
 * and the next writer creating its own at the same name, is the whole of the
 * setup, and an unconditional unlink at the end of the first transaction would
 * then take the second writer's lock, and the third writer's after that.
 * Nothing but the recorded pid can tell the two files apart here, which the
 * first assertion shows: `st_ino` on the boot root is derived from a hash of
 * the name, so two files that had one name in turn have one inode number.
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
 * closes when this process leaves, so if it fails before the byte goes out the
 * second writer's read returns end of file and it exits 22 instead of waiting.
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
 * luck; it must have waited. Its diagnostics go to a file so the parent can
 * check that a wait is announced once and not once per retry.
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
 * One refusal against a lock this probe wrote at `/etc/.pwlock`: the acquire
 * fails, the file is still there with its bytes unchanged, and the report says
 * which lock it is and what to do about it.
 *
 * The acquire waits out the compiled-in retry budget, so this is the path a
 * writer takes, with the lock path and the budget the shipped programs use
 * rather than parameters a test chose. Returns 0, or -1 after printing the
 * failure, which is `what` when the acquire succeeds.
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
 * The recovery rules at `/etc/.pwlock`, and the control that makes them mean
 * something: with the file gone, the same acquire succeeds.
 *
 * The lock names a reaped pid and is refused; then the same lock with its
 * modification time an hour back is refused as well, since nothing can tell an
 * abandoned holder from a stopped one and the library applies no age rule.
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
    /* The same lock, an hour old. */
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
 * Read from `fd` until `buffer` has `count` occurrences of `needle`, or `ms`
 * milliseconds pass. Returns 1 when they arrived and 0 otherwise.
 *
 * A caller that waits for a reprinted prompt needs the later copy and not the
 * first, since what it waits out is the flush that precedes the reprint, so the
 * occurrences already in the buffer are counted rather than matched.
 *
 * Every read of the master is bounded, because a probe that blocks on a
 * terminal nobody is writing to would hang the whole suite, not fail it.
 * A read of zero is waited through, not taken as the end: a master whose
 * slave is not open yet reads as end of file, and the child does its own open
 * after the fork.
 */
static int expect_nth_on(int fd, const char *needle, int count, char *buffer, size_t size, long ms)
{
    size_t filled = strlen(buffer);
    size_t len = strlen(needle);
    long waited = 0;

    while (waited < ms) {
        const char *at = buffer;
        int seen = 0;
        ssize_t n;

        while ((at = strstr(at, needle)) != NULL) {
            seen++;
            at += len;
        }
        if (seen >= count) {
            return 1;
        }
        n = read(fd, buffer + filled, size - 1 - filled);
        if (n > 0) {
            filled += (size_t)n;
            buffer[filled] = '\0';
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

/** Read from `fd` until `buffer` has `needle`, within `ms`. */
static int expect_on(int fd, const char *needle, char *buffer, size_t size, long ms)
{
    return expect_nth_on(fd, needle, 1, buffer, size, ms);
}

/**
 * Read `n` bytes of a leader's verdict from its pipe, within `ms`.
 *
 * A leader that hangs, waiting on a reader that neither stops nor ends, would
 * otherwise hold this probe until the harness kills the machine and the log
 * says nothing of which case it was; past the bound the leader is killed, so
 * the caller's `waitpid` returns and its diagnostic names the case.
 */
static ssize_t read_verdict(int fd, pid_t leader, char *buffer, size_t n, long ms)
{
    size_t filled = 0;
    long waited = 0;

    fcntl(fd, F_SETFL, O_NONBLOCK);
    while (filled < n && waited < ms) {
        ssize_t got = read(fd, buffer + filled, n - filled);

        if (got > 0) {
            filled += (size_t)got;
            continue;
        }
        if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            break;
        }
        nap(50);
        waited += 50;
    }
    if (filled < n) {
        kill(leader, SIGKILL);
    }
    return (ssize_t)filled;
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
 * The prompt's side of a terminal: read a password on the pseudo-terminal
 * slave `name` and exit 0 if it is `secret`.
 *
 * `setsid` and `TIOCSCTTY` are the handoff `login` and dropbear make, and they
 * are what puts the child in the foreground process group a `^Z` reaches. The
 * child exits 41 to 44 when a step before the comparison fails, and 45 when the
 * password is another one.
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
 * instead of letting it become a password byte; and the password read across
 * the whole cycle is what was typed after it.
 *
 * The stop handler puts the modes back before the process stops, so what is
 * typed at the stopped reader is echoed. The reader writes one newline of its
 * own when it puts the modes back after the read, so a blank line after the
 * password is expected and a character of the password is not.
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

    /* A stopped reader leaves a terminal that echoes. */
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
        /* Only the reader's own newline may come back. */
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
 * what was typed after the continue, so `X` becoming the first character of the
 * password is the failure, and a user meets it by typing `^Z` and a shell
 * command in one burst.
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
 * Make the pipe on which a session leader tells its reader that the terminal
 * is the reader's. Returns what `pipe` returns.
 *
 * The reader is forked before the leader has given it the terminal, and a
 * reader that printed its prompt first would have the master answering, with
 * an interrupt or a suspend character, a terminal the leader still owns: the
 * signal would land on the leader, which stops and never reports. So the
 * reader waits on this pipe for the leader's word, given once `tcsetpgrp` has
 * run.
 */
static int go_make(int go[2])
{
    return pipe(go);
}

/** The reader's side: wait for the leader's word, then close the pipe. */
static void go_wait(int go[2])
{
    char c;

    close(go[1]);
    (void)read(go[0], &c, 1);
    close(go[0]);
}

/** The leader's side: give the word, then close the pipe. */
static void go_give(int go[2])
{
    close(go[0]);
    (void)write(go[1], "g", 1);
    close(go[1]);
}

/**
 * The shell's side of a continue in the background: a session leader that owns
 * the terminal, a reader in a process group of its own, and a `bg`.
 *
 * It runs the whole case itself because the reader is its child and not the
 * probe's, and reports one line on `report`: the state of `ECHO` on the
 * terminal a second after the continue. A `bg` gives the terminal back to the
 * shell and only then continues the job. `TIOCSPGRP` raises no `SIGTTOU` on
 * this kernel, so taking the terminal back from a background group needs none
 * of the signal handling a shell puts around it. Exits 0 after reporting, and
 * 51 to 59 when a step fails before the report.
 */
static void prompt_bg_leader(const char *name, int master, int report)
{
    struct termios tio;
    char verdict[32];
    pid_t reader;
    int slave = open(name, O_RDWR);
    int status;
    int go[2];

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

    if (go_make(go) != 0) {
        _exit(54);
    }
    reader = fork();
    if (reader == 0) {
        char password[64];

        setpgid(0, 0);
        go_wait(go);
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
    go_give(go);
    if (waitpid(reader, &status, WUNTRACED) != reader || !WIFSTOPPED(status)) {
        _exit(56);
    }

    /* `bg`: the terminal goes back to the shell before the continue. */
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

/* Set by a reader before it prompts; the next `tcgetpgrp` clears it and stops
 * the reader with `SIGTSTP`. */
static volatile sig_atomic_t Tcg_stop;

/**
 * This image's `tcgetpgrp(3)`, which stops the reader the moment it has its
 * answer when `Tcg_stop` is set. Returns the group, or -1 with `errno` set.
 *
 * The library asks who owns the terminal and then acts on the answer, and the
 * answer goes stale if a stop is served in between: a shell can reclaim the
 * terminal and continue the reader in the background, after which acting on the
 * old answer silences a terminal the reader no longer owns. That interval
 * cannot be entered from outside, so it is entered from within. It raises
 * `SIGTSTP` rather than `SIGSTOP`: `SIGTSTP` is the one a keystroke sends and
 * the one the library can defer, and a test using the unblockable signal would
 * check a guarantee the library does not give.
 *
 * It answers from `TIOCGPGRP`, the ioctl musl's own wrapper uses, so nothing is
 * lost by standing in for it.
 */

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
    int go[2];

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

    if (go_make(go) != 0) {
        _exit(54);
    }
    reader = fork();
    if (reader == 0) {
        char password[64];

        setpgid(0, 0);
        go_wait(go);
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
    go_give(go);
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
 * user typed. Dropping that one turns a correct password into a refusal, which
 * is a fault the user cannot see the cause of: they type the right thing and
 * are told it is wrong.
 *
 * The stub stops the reader at that point. Unlike the stale-owner case the
 * terminal stays with the reader, so what follows is an ordinary `fg` and an
 * ordinary password, and every character of it must survive.
 *
 * The continue is not the moment to type. The resume re-applies the echo-off
 * modes with `TCSAFLUSH`, which discards whatever is typed already, and the
 * mode change the stop interrupted does it once more as it returns; each
 * flush is followed by a prompt, so a password typed after the second prompt
 * is the first one no flush can take. Typing on the leader's report alone
 * would lose the password whenever the reader had not reached those flushes
 * yet, and no scheduling order rules that out, so the case waits for the
 * second prompt.
 */
static int check_prompt_stop_before_read(void)
{
    char verdict[8];
    char prompts[128];
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
    if (read_verdict(channel[0], leader, verdict, 1, 10000) != 1 || verdict[0] != 'r') {
        close(master);
        close(channel[0]);

        return fail("the pre-read stop case never saw its reader resume");
    }
    prompts[0] = '\0';
    if (!expect_nth_on(master, "Password: ", 2, prompts, sizeof(prompts), 10000)) {
        kill(leader, SIGKILL);
        close(master);
        close(channel[0]);
        (void)waitpid(leader, &status, 0);

        return fail("the pre-read stop case never saw the prompt its reader reprints");
    }
    if (write(master, "secret\n", 7) != 7) {
        close(master);
        close(channel[0]);

        return fail("could not type the password the pre-read stop case checks");
    }
    if (read_verdict(channel[0], leader, verdict, 1, 10000) != 1) {
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
    int go[2];

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

    if (go_make(go) != 0) {
        _exit(54);
    }
    reader = fork();
    if (reader == 0) {
        char password[64];

        setpgid(0, 0);
        go_wait(go);
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
    go_give(go);
    /* The stub stops it inside the library, at its `tcgetpgrp`. */
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
 * The ownership check opens this window unless asking and acting are one step.
 * The reader is the foreground group when it asks, is stopped before it can
 * act, and is continued in the background by a leader that has taken the
 * terminal back. Acting on the stale answer silences a terminal belonging to
 * somebody else, which is the same damage as the background case and reachable
 * the same way, by one `^Z` at a password prompt.
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
    got = read_verdict(channel[0], leader, verdict, 2, 20000);
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
 * The child for the overflow-then-resume case: one read on `in`, which must
 * come back with the line typed after the stop. Exits 0 for `secret`, 44 when
 * the read fails and 45 for another line.
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
 * The reader refuses an over-long entry instead of storing the prefix it
 * fitted, and it drops the line it has whenever a resume is served. Those two
 * meet here: a resume starts a fresh line, so the overflow that ended the old
 * one must go with it. Left standing, the flag rejects the password that fits,
 * typed after the continue, and the user is told their entry was too long when
 * it was not, with no way past it but to start the program again.
 *
 * The channel is a pipe and not a pseudo-terminal, which is why this case can
 * exist. A terminal here is canonical, so nothing reaches the reader until a
 * newline does and an over-long line therefore ends the read before any stop
 * can be served; on a pipe the bytes arrive as they are written, so the reader
 * can be holding an over-long run when the stop lands. Driving a program by
 * script, which is how these probes and dropbear both run one, is that case.
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

    /* Too long to keep, with no newline, so the reader is still collecting. */
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
    /* No `tcsetpgrp`: the reader is a background group throughout. */
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
 * So it is asked here. Both halves are asserted: the reader stopped on
 * `SIGTTIN` instead of being handed a byte, and the terminal still echoes.
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
    got = read_verdict(channel[0], leader, verdict, 2, 20000);
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
 * and its default action ends the process. The prompt is written to file
 * descriptor 2 after echo has been taken away, so that death would leave the
 * console silenced with no program left to restore it; and a writer's
 * diagnostic reaches the same write with the lock held, where the death runs
 * no `atexit` and the lock file stays in `/etc`. So the cases are the two
 * lifetimes, and each is a child, because what is checked for is a death, not
 * a return value. The writer's child leaves through `exit`, so the release
 * must have run: a lock still at the name afterwards is the stranded lock this
 * case is about.
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
 * so it has to be entered from a caller whose real uid is 1000; and a refused
 * `su` must leave that caller as it was, which only the caller can say. It
 * exits with the low seven bits of the run's status, with `CALLER_SIGNAL` when
 * the run died of a signal, and with `CALLER_BROKEN` when a step of its own
 * failed or the run left the caller's identity changed.
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
 * The signals above the host's range, and the fault signals sent with `kill`.
 *
 * The library handles as a death every signal this kernel terminates a process
 * for, over the whole range 1..64, except the few `pwauth_is_death` leaves out.
 * The host test checks that set against the dispositions installed, but Darwin
 * stops at 32, so the upper half is checked here. A signal in the upper range
 * must be handled like any other death: the writer leaves through `exit` and
 * its lock is gone. And 32, 33 and 34 must not be handled, because musl
 * reserves them and its `sigaction` fails for them; a writer sent one dies of
 * it with the lock behind, as `SIGKILL` leaves one, and `pwauth.h` lists them.
 *
 * A CPU exception on this kernel is a forced signal (`signal_force`), which
 * reaches an installed handler, and a `SIGSEGV`, `SIGILL` or `SIGFPE` can also
 * be sent by another process; the library handles both as a death. This case
 * sends them, and the writer must leave through `exit` as for any other.
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
            if (waitpid(child, &status, WNOHANG) == child) {
                return fail("the writer the high-signal case signals did not begin");
            }
            sleep(1);
        }
        kill(child, handled[i]);
        if (waitpid(child, &status, 0) != child) {
            return fail("could not reap the writer the high-signal case signals");
        }
        if (WIFSIGNALED(status)) {
            return fail("a writer sent a handled signal took the default action");
        }
        if (!WIFEXITED(status)) {
            return fail("a writer sent a handled signal ended in no known way");
        }
        if (present("/etc/.pwlock")) {
            unlink("/etc/.pwlock");

            return fail("a writer sent a handled signal left its lock behind");
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
 * `su` itself: the non-login run, two refused ones, and the login run.
 *
 * The two successful runs differ in one argument and in nothing else, so what
 * the login run shows (a working directory of the target's home and a
 * leading-dash `argv[0]`) is what the non-login run shows is not done. The
 * caller's own variable is in the environment of both, and reaches neither.
 *
 * The first refusal is of the account's own password made longer than
 * `PWAUTH_MAX_PASSWORD`, which `passwd` refuses to set and only a written
 * record can arrange. The limit is the library's, whatever buffer a caller
 * declares: a password `su` accepted and dropbear refuses is an account that
 * works at the console and never over the network. A long wrong password would
 * show nothing, since it is refused for being wrong; the long right one asks
 * whether the reader stops at the limit every other reader in the image stops
 * at. The second refusal is of another account's password.
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

    /* A password longer than any reader in the image takes. */
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

        /* The account's own password, made that long in the shadow file. */
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

/**
 * Snapshot the databases, run the cases in order until one fails, and clean
 * up. Returns 0 on success and 1 otherwise.
 */
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
