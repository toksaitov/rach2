/**
 * `passwd` in the guest: the first program that writes a credential file.
 *
 * link-deps: utils/pwauth.c
 *
 * The library's own probe covers the transaction; this covers the program built
 * on it, and it drives the shipped `/passwd` at the shipped mode, not a
 * copy, so what is proved is what a user runs. Its cases, in the order they
 * run:
 *
 * The sequential arc, asserted on the file's bytes and metadata, not on
 * an exit status. A uid-1000 child feeds its old and new passwords to `/passwd`
 * on a pipe and the account's hash must change to a `$6$` with a different
 * salt, `crypt` must reproduce it from the new password, every other line must
 * be byte-identical (a check through the account alone would pass on a file
 * that lost every other one), the mode and owner must still be 0600 root, and
 * no temporary file and no lock may remain.
 *
 * Then the refusals, each of which must change nothing at all: a wrong old
 * password; uid 1000 naming another account, which is the real-uid rule and the
 * milestone-16 `su` finding in a second place, since this binary is 04755 and a
 * test on the effective uid would authorize everyone; and a database naming one
 * account twice, which musl's parsers resolve by file order and this program
 * refuses outright, because the line that authenticates and the line that is
 * rewritten could otherwise be different ones.
 *
 * Then the concurrent case, which is what the lock exists for and what no
 * single-process test can reach: two accounts changed at once, where the loser
 * must wait, not read a pre-image and publish over the winner's line.
 * The rendezvous is a token on the loser's standard error and never a sleep.
 *
 * Then the entropy implication, and it is `getrandom`'s answer that decides
 * which side runs, never a CPUID bit: a mint that cannot be trusted must refuse
 * before the new password is asked for, so that nothing is collected that
 * cannot be used.
 *
 * Prints "PASSWDMUSL OK" on success and "PASSWDMUSL FAIL (detail)" otherwise.
 * It restores all three databases byte for byte before it exits, since the
 * probes share one boot and the ones after this read the same files.
 */

#define _GNU_SOURCE /* crypt, getrandom */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <crypt.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pwauth.h"

#define PASSWD_PATH "/passwd"
#define ETC_PASSWD  "/etc/passwd"
#define ETC_SHADOW  "/etc/shadow"
#define ETC_GROUP   "/etc/group"

/* The two unprivileged accounts the test image stages, with the passwords whose
 * hashes are committed in the manifest. Two of them, because a lost update is
 * only visible when two accounts change. */
#define PROBE_USER  "probe"
#define PROBE_PASS  "probepass"
#define PROBE2_USER "probe2"
#define PROBE2_PASS "probe2pass"

#define PROBE_UID  1000
#define PROBE2_UID 1001

static const char *Databases[3] = {ETC_PASSWD, ETC_SHADOW, ETC_GROUP};
static char Snapshot[3][4096];
static long Snapshot_len[3];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PASSWDMUSL FAIL (%s)\n", what);

    return 1;
}

/** Read the whole of `path` into `buffer`. Returns the length, or -1. */
static long slurp(const char *path, char *buffer, size_t size)
{
    ssize_t got;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    got = read(fd, buffer, size - 1);
    close(fd);
    if (got < 0) {
        return -1;
    }
    buffer[got] = '\0';

    return (long)got;
}

/** Write `text` to `path` at `mode`, creating or truncating it. Returns 0. */
static int spill(const char *path, const char *text, mode_t mode)
{
    size_t len = strlen(text);
    ssize_t wrote;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (fd < 0) {
        return -1;
    }
    wrote = write(fd, text, len);
    close(fd);

    return wrote == (ssize_t)len ? 0 : -1;
}

/** True when something exists at `path`. */
static int present(const char *path)
{
    struct stat info;

    return stat(path, &info) == 0;
}

/**
 * Copy the line naming `user` out of `text` into `out`, without its newline.
 * Returns 0 when exactly one line named it.
 */
static int line_of(const char *text, const char *user, char *out, size_t size)
{
    size_t len = strlen(user);
    const char *line = text;
    int found = 0;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            if (found++ == 0 && span < size) {
                memcpy(out, line, span);
                out[span] = '\0';
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return found == 1 ? 0 : -1;
}

/** Copy the hash field out of a shadow line. Returns 0 on success. */
static int hash_of_line(const char *line, char *out, size_t size)
{
    const char *colon = strchr(line, ':');
    const char *stop;
    size_t len;

    if (!colon) {
        return -1;
    }
    colon++;
    stop = strchr(colon, ':');
    len = stop ? (size_t)(stop - colon) : strlen(colon);
    if (len >= size) {
        return -1;
    }
    memcpy(out, colon, len);
    out[len] = '\0';

    return 0;
}

/**
 * The same text with `user`'s line removed. Used to compare everything the
 * change did not touch, since an account that verifies says nothing about the
 * lines beside it.
 */
static void without(const char *text, const char *user, char *out, size_t size)
{
    size_t len = strlen(user);
    const char *line = text;
    size_t off = 0;

    out[0] = '\0';
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);
        int mine = span > len && strncmp(line, user, len) == 0 && line[len] == ':';

        if (!mine && span > 0 && off + span + 2 < size) {
            memcpy(out + off, line, span);
            off += span;
            out[off++] = '\n';
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    out[off] = '\0';
}

/**
 * The hash field of `user`'s line in `text`, as a pointer into a static buffer.
 * Used only to rebuild a record the malformed case then breaks in one field.
 */
static const char *hash_from(const char *text, const char *user)
{
    static char line[512];
    static char hash[256];

    if (line_of(text, user, line, sizeof(line)) != 0 ||
        hash_of_line(line, hash, sizeof(hash)) != 0) {
        return "";
    }

    return hash;
}

/**
 * Put the three databases back as the image staged them.
 *
 * Called at the head of every case, not only at the end, because each
 * case is written against the staged passwords and a case that ran before it
 * has changed one. Without this the refusals would pass for the wrong reason:
 * a wrong-password case and a wrong-caller case both refuse just as readily
 * when the password in the script is simply out of date, and the case would be
 * asserting nothing about the rule it names.
 */
static void restore_databases(void)
{
    size_t i;

    for (i = 0; i < 3; i++) {
        (void)spill(Databases[i], Snapshot[i], i == 1 ? 0600 : 0644);
    }
    (void)unlink(CRED_LOCK_PATH);
}

/**
 * Read from `fd` until `needle` appears or `ms` elapse. Returns 1 on a match.
 *
 * The rendezvous every case here uses in place of a sleep: a token the other
 * process could only have printed after doing the thing being waited for. The
 * deadline bounds a failure and never decides a pass, since the pass condition
 * is the token and a build that printed a different one fails on the deadline
 * instead of passing on it.
 */
static int read_until(int fd, const char *needle, char *buffer, size_t size, long ms)
{
    size_t held = strlen(buffer);
    long waited = 0;

    while (waited < ms) {
        struct pollfd waiting;
        ssize_t got;

        waiting.fd = fd;
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, 200) == 1) {
            got = read(fd, buffer + held, size - held - 1);
            if (got <= 0) {
                return 0;
            }
            held += (size_t)got;
            buffer[held] = '\0';
            if (strstr(buffer, needle) != NULL) {
                return 1;
            }
            if (held + 1 >= size) {
                return 0;
            }
            continue;
        }
        waited += 200;
    }

    return 0;
}

/**
 * Run `/passwd` as `uid`, feeding `script` to its standard input and collecting
 * its standard error into `report`. Returns the child's exit status, or -1.
 *
 * The input is a pipe, not a terminal, which is what makes the script
 * possible: the prompt takes the non-tty path and reads the lines as they come.
 */
static int run_passwd_raw(uid_t uid, const char *user, const char *script, size_t script_len,
                          char *report, size_t report_size);

/**
 * As `run_passwd_raw`, for a script that is an ordinary C string.
 */
static int run_passwd(uid_t uid, const char *user, const char *script, char *report,
                      size_t report_size)
{
    return run_passwd_raw(uid, user, script, script ? strlen(script) : 0, report, report_size);
}

/**
 * Remove `user`'s line from `text` in place. Returns 0 when one was there.
 */
static int strip_line(char *text, const char *user)
{
    size_t len = strlen(user);
    char *line = text;

    while (*line) {
        char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            char *rest = end ? end + 1 : line + span;

            memmove(line, rest, strlen(rest) + 1);

            return 0;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return -1;
}

/**
 * Splice `value` into the aging field of `user`'s shadow line in `text`, in
 * place. The caller's buffer must have room for the growth. Returns 0 when it
 * was done.
 */
static int spoil_aging(char *text, const char *user, const char *value)
{
    size_t len = strlen(user);
    char *line = text;

    while (*line) {
        char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            char *field = strchr(line + len + 1, ':');
            char *stop = field ? strchr(field + 1, ':') : NULL;

            if (!field || !stop) {
                return -1;
            }
            memmove(field + 1 + strlen(value), stop, strlen(stop) + 1);
            memcpy(field + 1, value, strlen(value));

            return 0;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return -1;
}

/**
 * Run `/passwd` as `uid`, feeding it `script_len` bytes and collecting its
 * standard error. Returns the child's exit status, or -1.
 *
 * The length is a parameter because one case feeds a script containing a NUL,
 * which is exactly the byte `strlen` would stop at, and that case exists
 * because the program must not stop at it either.
 */
static int run_passwd_raw(uid_t uid, const char *user, const char *script, size_t script_len,
                          char *report, size_t report_size)
{
    int in[2];
    int err[2];
    pid_t child;
    int status;
    ssize_t got;
    size_t held = 0;

    if (report) {
        report[0] = '\0';
    }
    if (pipe(in) != 0) {
        return -1;
    }
    if (pipe(err) != 0) {
        close(in[0]);
        close(in[1]);

        return -1;
    }
    child = fork();
    if (child == 0) {
        close(in[1]);
        close(err[0]);
        if (dup2(in[0], 0) < 0 || dup2(err[1], 2) < 0) {
            _exit(90);
        }
        if (in[0] > 2) {
            close(in[0]);
        }
        if (err[1] > 2) {
            close(err[1]);
        }
        if (uid != 0 && setuid(uid) != 0) {
            _exit(91);
        }
        if (user) {
            execl(PASSWD_PATH, "passwd", user, (char *)0);
        } else {
            execl(PASSWD_PATH, "passwd", (char *)0);
        }
        _exit(92);
    }
    close(in[0]);
    close(err[1]);
    if (child < 0) {
        close(in[1]);
        close(err[0]);

        return -1;
    }
    if (script && script_len > 0) {
        (void)write(in[1], script, script_len);
    }
    close(in[1]);
    if (report) {
        while (held + 1 < report_size &&
               (got = read(err[0], report + held, report_size - held - 1)) > 0) {
            held += (size_t)got;
        }
        report[held] = '\0';
    }
    close(err[0]);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

/**
 * The change itself, asserted on the file, not on a status.
 */
static int check_change(void)
{
    char before[4096];
    char after[4096];
    char rest_before[4096];
    char rest_after[4096];
    char line[512];
    char old_hash[256];
    char new_hash[256];
    char script[256];
    char report[512];
    struct stat info;
    char *made;
    int status;

    restore_databases();

    if (slurp(ETC_SHADOW, before, sizeof(before)) < 0) {
        return fail("cannot read the shadow file the change case rewrites");
    }
    if (line_of(before, PROBE_USER, line, sizeof(line)) != 0 ||
        hash_of_line(line, old_hash, sizeof(old_hash)) != 0) {
        return fail("the shadow file does not name the probe account once");
    }

    snprintf(script, sizeof(script), "%s\nnew-probe-pw\nnew-probe-pw\n", PROBE_PASS);
    status = run_passwd(PROBE_UID, NULL, script, report, sizeof(report));
    if (status != 0) {
        char said[640];

        /* The program's own line, not just the status: a refusal here has half
         * a dozen causes and the message names which. */
        snprintf(said, sizeof(said), "passwd refused a correct old password (status %d, said: %s)",
                 status, report);

        return fail(said);
    }

    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0) {
        return fail("cannot read the shadow file passwd published");
    }
    if (line_of(after, PROBE_USER, line, sizeof(line)) != 0 ||
        hash_of_line(line, new_hash, sizeof(new_hash)) != 0) {
        return fail("the published shadow file does not name the account once");
    }
    if (strncmp(new_hash, "$6$", 3) != 0) {
        return fail("passwd published a hash that is not SHA-512 crypt");
    }
    if (strcmp(new_hash, old_hash) == 0) {
        return fail("passwd published the hash it was asked to replace");
    }
    made = crypt("new-probe-pw", new_hash);
    if (!made || strcmp(made, new_hash) != 0) {
        return fail("the published hash is not the new password's");
    }
    /* The salt is minted per change, so two changes of one password differ. */
    if (strncmp(new_hash, old_hash, 12) == 0) {
        return fail("passwd reused the salt it found");
    }

    without(before, PROBE_USER, rest_before, sizeof(rest_before));
    without(after, PROBE_USER, rest_after, sizeof(rest_after));
    if (strcmp(rest_before, rest_after) != 0) {
        return fail("passwd changed a line that was not the account's");
    }
    if (stat(ETC_SHADOW, &info) != 0) {
        return fail("cannot stat the shadow file passwd published");
    }
    if ((info.st_mode & 07777) != 0600 || info.st_uid != 0 || info.st_gid != 0) {
        return fail("passwd published a shadow file with the wrong mode or owner");
    }
    if (present(CRED_LOCK_PATH)) {
        return fail("passwd left its lock behind");
    }
    if (present("/etc/shadow.tmp") || present("/etc/passwd.tmp")) {
        return fail("passwd left a temporary file behind");
    }

    return 0;
}

/**
 * Two writers overlapping, which is the whole reason for the lock.
 *
 * `passwd` prompts first and takes the transaction last, so the lock is held
 * for microseconds, not for as long as a person takes to choose a
 * password. What keeps that safe is that it re-reads the database under the
 * lock and rewrites THAT, not the copy it surveyed before prompting,
 * and that it refuses if the record it authenticated against is no longer the
 * one it is about to replace. Both halves are asserted here, and the sequencing
 * is exact, not hopeful: the second writer is made to prompt first, so
 * it is holding a pre-image by construction, and only then is the first writer
 * run to completion.
 *
 * Two accounts first, where both changes must survive: the writer that
 * published second re-read the other's result and carried it through. The image
 * has `probe2` at uid 1001 for exactly this, since a lost update is only
 * visible when two accounts change.
 *
 * Then one account changed by two writers, where the second must be refused.
 * Its pre-image is stale in the one way that matters, and publishing from it
 * would erase a change its caller never saw.
 */
static int check_concurrent(void)
{
    char after[4096];
    char before[4096];
    char line[512];
    char hash[256];
    char report[512];
    char waiting[512];
    char script[256];
    int b_in[2];
    int b_err[2];
    pid_t b;
    int status;
    char *made;

    restore_databases();

    /* The second writer, started first and held at its new-password prompt. It
     * has read and authenticated by then, so its survey is the pre-image. */
    if (pipe(b_in) != 0 || pipe(b_err) != 0) {
        return fail("could not make the pipes the overlapping writer reports on");
    }
    b = fork();
    if (b == 0) {
        close(b_in[1]);
        close(b_err[0]);
        if (dup2(b_in[0], 0) < 0 || dup2(b_err[1], 2) < 0) {
            _exit(90);
        }
        if (setuid(PROBE2_UID) != 0) {
            _exit(91);
        }
        execl(PASSWD_PATH, "passwd", (char *)0);
        _exit(92);
    }
    close(b_in[0]);
    close(b_err[1]);
    if (b < 0) {
        return fail("could not fork the overlapping writer");
    }
    (void)write(b_in[1], PROBE2_PASS "\n", strlen(PROBE2_PASS) + 1);
    waiting[0] = '\0';
    if (!read_until(b_err[0], "New password:", waiting, sizeof(waiting), 20000)) {
        kill(b, SIGKILL);
        (void)waitpid(b, &status, 0);

        return fail("the overlapping writer never reached its new-password prompt");
    }

    /* The first writer runs to completion while the second holds its
     * pre-image. */
    snprintf(script, sizeof(script), "%s\nfirst-new-pw\nfirst-new-pw\n", PROBE_PASS);
    if (run_passwd(PROBE_UID, NULL, script, report, sizeof(report)) != 0) {
        kill(b, SIGKILL);
        (void)waitpid(b, &status, 0);

        return fail("the writer that published first did not finish cleanly");
    }

    /* Only now does the second publish, from a survey that is out of date. */
    (void)write(b_in[1], "second-new-pw\nsecond-new-pw\n", 28);
    close(b_in[1]);
    if (waitpid(b, &status, 0) != b || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the overlapping writer did not finish cleanly");
    }
    close(b_err[0]);

    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0) {
        return fail("cannot read the shadow file both writers published to");
    }
    if (line_of(after, PROBE_USER, line, sizeof(line)) != 0 ||
        hash_of_line(line, hash, sizeof(hash)) != 0) {
        return fail("the first writer's account is not named once afterwards");
    }
    made = crypt("first-new-pw", hash);
    if (!made || strcmp(made, hash) != 0) {
        return fail("the writer that published second erased the first one's change");
    }
    if (line_of(after, PROBE2_USER, line, sizeof(line)) != 0 ||
        hash_of_line(line, hash, sizeof(hash)) != 0) {
        return fail("the second writer's account is not named once afterwards");
    }
    made = crypt("second-new-pw", hash);
    if (!made || strcmp(made, hash) != 0) {
        return fail("the second writer's own change was lost");
    }
    if (line_of(after, "root", line, sizeof(line)) != 0) {
        return fail("root's line did not survive two overlapping writers");
    }
    if (present(CRED_LOCK_PATH)) {
        (void)unlink(CRED_LOCK_PATH);

        return fail("a lock survived two overlapping writers");
    }

    /* The same account, changed by two writers. The one holding the stale
     * record must be refused, not publish over a change it never saw. */
    restore_databases();
    if (pipe(b_in) != 0 || pipe(b_err) != 0) {
        return fail("could not make the pipes the same-account case reports on");
    }
    b = fork();
    if (b == 0) {
        close(b_in[1]);
        close(b_err[0]);
        if (dup2(b_in[0], 0) < 0 || dup2(b_err[1], 2) < 0) {
            _exit(90);
        }
        if (setuid(PROBE_UID) != 0) {
            _exit(91);
        }
        execl(PASSWD_PATH, "passwd", (char *)0);
        _exit(92);
    }
    close(b_in[0]);
    close(b_err[1]);
    if (b < 0) {
        return fail("could not fork the writer that must be refused");
    }
    (void)write(b_in[1], PROBE_PASS "\n", strlen(PROBE_PASS) + 1);
    waiting[0] = '\0';
    if (!read_until(b_err[0], "New password:", waiting, sizeof(waiting), 20000)) {
        kill(b, SIGKILL);
        (void)waitpid(b, &status, 0);

        return fail("the writer that must be refused never reached its prompt");
    }
    /* Root changes the same account out from under it. */
    snprintf(script, sizeof(script), "root-set-pw\nroot-set-pw\n");
    if (run_passwd(0, PROBE_USER, script, report, sizeof(report)) != 0) {
        kill(b, SIGKILL);
        (void)waitpid(b, &status, 0);

        return fail("root could not change the account the stale writer holds");
    }
    (void)write(b_in[1], "stale-new-pw\nstale-new-pw\n", 26);
    close(b_in[1]);
    waiting[0] = '\0';
    (void)read_until(b_err[0], "changed while", waiting, sizeof(waiting), 20000);
    if (waitpid(b, &status, 0) != b || !WIFEXITED(status)) {
        return fail("the writer that must be refused did not exit normally");
    }
    close(b_err[0]);
    if (WEXITSTATUS(status) == 0) {
        return fail("a writer published from a record that had changed under it");
    }
    if (strstr(waiting, "changed while") == NULL) {
        return fail("a writer refused for a changed record did not say so");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 ||
        line_of(after, PROBE_USER, line, sizeof(line)) != 0 ||
        hash_of_line(line, hash, sizeof(hash)) != 0) {
        return fail("cannot read back the account the two writers contended for");
    }
    made = crypt("root-set-pw", hash);
    if (!made || strcmp(made, hash) != 0) {
        return fail("the refused writer published over root's change anyway");
    }

    /* The caller's claim, revoked while the password is being typed. A name is
     * not owned forever: an administrator can remove the account or give the
     * uid to somebody else, and none of that need touch the shadow hash, so a
     * writer that rechecked only the record would publish as root for an
     * account that had just stopped being theirs. */
    restore_databases();
    if (pipe(b_in) != 0 || pipe(b_err) != 0) {
        return fail("could not make the pipes the revocation case reports on");
    }
    b = fork();
    if (b == 0) {
        close(b_in[1]);
        close(b_err[0]);
        if (dup2(b_in[0], 0) < 0 || dup2(b_err[1], 2) < 0) {
            _exit(90);
        }
        if (setuid(PROBE_UID) != 0) {
            _exit(91);
        }
        execl(PASSWD_PATH, "passwd", (char *)0);
        _exit(92);
    }
    close(b_in[0]);
    close(b_err[1]);
    if (b < 0) {
        return fail("could not fork the writer whose claim is revoked");
    }
    (void)write(b_in[1], PROBE_PASS "\n", strlen(PROBE_PASS) + 1);
    waiting[0] = '\0';
    if (!read_until(b_err[0], "New password:", waiting, sizeof(waiting), 20000)) {
        kill(b, SIGKILL);
        (void)waitpid(b, &status, 0);

        return fail("the writer whose claim is revoked never reached its prompt");
    }
    /* The uid stops naming that account, with the shadow record left alone. */
    {
        char passwd_text[4096];
        char rest[4096];
        char revoked[4096];

        if (slurp(ETC_PASSWD, passwd_text, sizeof(passwd_text)) < 0) {
            kill(b, SIGKILL);
            (void)waitpid(b, &status, 0);

            return fail("cannot read the passwd file the revocation case rewrites");
        }
        without(passwd_text, PROBE_USER, rest, sizeof(rest));
        if ((size_t)snprintf(revoked, sizeof(revoked),
                             "%s%s:x:%d:%d:renamed:/home/probe:/idreport\n", rest, PROBE_USER "x",
                             PROBE_UID, PROBE_UID) >= sizeof(revoked)) {
            kill(b, SIGKILL);
            (void)waitpid(b, &status, 0);

            return fail("the revoked passwd file does not fit");
        }
        if (spill(ETC_PASSWD, revoked, 0644) != 0) {
            kill(b, SIGKILL);
            (void)waitpid(b, &status, 0);

            return fail("could not stage the revoked passwd file");
        }
    }
    (void)write(b_in[1], "revoked-new-pw\nrevoked-new-pw\n", 30);
    close(b_in[1]);
    waiting[0] = '\0';
    (void)read_until(b_err[0], "no longer names", waiting, sizeof(waiting), 20000);
    if (waitpid(b, &status, 0) != b || !WIFEXITED(status)) {
        return fail("the writer whose claim is revoked did not exit normally");
    }
    close(b_err[0]);
    if (WEXITSTATUS(status) == 0) {
        return fail("a writer published for an account its uid no longer names");
    }
    if (strstr(waiting, "no longer names") == NULL) {
        return fail("a writer refused for a revoked claim did not say so");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 ||
        line_of(after, PROBE_USER, line, sizeof(line)) != 0 ||
        hash_of_line(line, hash, sizeof(hash)) != 0) {
        return fail("cannot read back the account whose claim was revoked");
    }
    made = crypt("revoked-new-pw", hash);
    if (made && strcmp(made, hash) == 0) {
        return fail("the revoked writer published its new password anyway");
    }

    /* The same revocation done by ADDING a line instead of renaming one. The
     * caller's uid still resolves to the same name, so a check that asked only
     * that question would pass; but the login now names two accounts and
     * `getpwnam` takes the first, so the hash this caller publishes would
     * authenticate a uid that is not theirs. */
    restore_databases();
    if (pipe(b_in) != 0 || pipe(b_err) != 0) {
        return fail("could not make the pipes the reassignment case reports on");
    }
    b = fork();
    if (b == 0) {
        close(b_in[1]);
        close(b_err[0]);
        if (dup2(b_in[0], 0) < 0 || dup2(b_err[1], 2) < 0) {
            _exit(90);
        }
        if (setuid(PROBE_UID) != 0) {
            _exit(91);
        }
        execl(PASSWD_PATH, "passwd", (char *)0);
        _exit(92);
    }
    close(b_in[0]);
    close(b_err[1]);
    if (b < 0) {
        return fail("could not fork the writer whose login is reassigned");
    }
    (void)write(b_in[1], PROBE_PASS "\n", strlen(PROBE_PASS) + 1);
    waiting[0] = '\0';
    if (!read_until(b_err[0], "New password:", waiting, sizeof(waiting), 20000)) {
        kill(b, SIGKILL);
        (void)waitpid(b, &status, 0);

        return fail("the writer whose login is reassigned never reached its prompt");
    }
    {
        char passwd_text[4096];
        char doubled[4096];

        if (slurp(ETC_PASSWD, passwd_text, sizeof(passwd_text)) < 0) {
            kill(b, SIGKILL);
            (void)waitpid(b, &status, 0);

            return fail("cannot read the passwd file the reassignment case rewrites");
        }
        if ((size_t)snprintf(doubled, sizeof(doubled), "%s:x:%d:%d:other:/home/probe:/idreport\n%s",
                             PROBE_USER, PROBE2_UID, PROBE2_UID, passwd_text) >= sizeof(doubled)) {
            kill(b, SIGKILL);
            (void)waitpid(b, &status, 0);

            return fail("the reassigned passwd file does not fit");
        }
        if (spill(ETC_PASSWD, doubled, 0644) != 0) {
            kill(b, SIGKILL);
            (void)waitpid(b, &status, 0);

            return fail("could not stage the reassigned passwd file");
        }
    }
    (void)write(b_in[1], "reassigned-pw\nreassigned-pw\n", 28);
    close(b_in[1]);
    waiting[0] = '\0';
    (void)read_until(b_err[0], "no longer names", waiting, sizeof(waiting), 20000);
    if (waitpid(b, &status, 0) != b || !WIFEXITED(status)) {
        return fail("the writer whose login is reassigned did not exit normally");
    }
    close(b_err[0]);
    if (WEXITSTATUS(status) == 0) {
        return fail("a writer published for a login that names two accounts");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 ||
        line_of(after, PROBE_USER, line, sizeof(line)) != 0 ||
        hash_of_line(line, hash, sizeof(hash)) != 0) {
        return fail("cannot read back the account whose login was reassigned");
    }
    made = crypt("reassigned-pw", hash);
    if (made && strcmp(made, hash) == 0) {
        return fail("the writer published a hash that would authenticate another uid");
    }

    /* The record's SHAPE can rot while the prompts are open just as its hash
     * can, and a shape check made before the prompt does not cover it: the
     * aging fields and the final newline are copied through by the rewrite, so
     * a record that went malformed mid-prompt would be republished malformed
     * under a success message, for an account `getspnam` then refuses. */
    {
        static const char *const rotted[] = {"aging", "newline"};
        size_t which;

        for (which = 0; which < sizeof(rotted) / sizeof(rotted[0]); which++) {
            restore_databases();
            if (pipe(b_in) != 0 || pipe(b_err) != 0) {
                return fail("could not make the pipes the shape case reports on");
            }
            b = fork();
            if (b == 0) {
                close(b_in[1]);
                close(b_err[0]);
                if (dup2(b_in[0], 0) < 0 || dup2(b_err[1], 2) < 0) {
                    _exit(90);
                }
                if (setuid(PROBE_UID) != 0) {
                    _exit(91);
                }
                execl(PASSWD_PATH, "passwd", (char *)0);
                _exit(92);
            }
            close(b_in[0]);
            close(b_err[1]);
            if (b < 0) {
                return fail("could not fork the writer whose record changes shape");
            }
            (void)write(b_in[1], PROBE_PASS "\n", strlen(PROBE_PASS) + 1);
            waiting[0] = '\0';
            if (!read_until(b_err[0], "New password:", waiting, sizeof(waiting), 20000)) {
                kill(b, SIGKILL);
                (void)waitpid(b, &status, 0);

                return fail("the writer whose record changes shape never reached its prompt");
            }
            {
                char shadow_text[4096];

                if (slurp(ETC_SHADOW, shadow_text, sizeof(shadow_text)) < 0) {
                    kill(b, SIGKILL);
                    (void)waitpid(b, &status, 0);

                    return fail("cannot read the shadow file the shape case rewrites");
                }
                if (strcmp(rotted[which], "aging") == 0) {
                    if (spoil_aging(shadow_text, PROBE_USER, "1x") != 0) {
                        kill(b, SIGKILL);
                        (void)waitpid(b, &status, 0);

                        return fail("could not spoil the aging field");
                    }
                } else {
                    char record[512];
                    size_t used;

                    /* Last, and unterminated: `getspnam` reports ERANGE for a
                     * matching record whose last character is not a newline,
                     * and only the last line of a file can lack one. */
                    if (line_of(shadow_text, PROBE_USER, record, sizeof(record)) != 0 ||
                        strip_line(shadow_text, PROBE_USER) != 0) {
                        kill(b, SIGKILL);
                        (void)waitpid(b, &status, 0);

                        return fail("could not move the record to the end of the shadow file");
                    }
                    used = strlen(shadow_text);
                    if (used + strlen(record) + 1 >= sizeof(shadow_text)) {
                        kill(b, SIGKILL);
                        (void)waitpid(b, &status, 0);

                        return fail("the unterminated shadow file does not fit");
                    }
                    memcpy(shadow_text + used, record, strlen(record) + 1);
                }
                if (spill(ETC_SHADOW, shadow_text, 0600) != 0) {
                    kill(b, SIGKILL);
                    (void)waitpid(b, &status, 0);

                    return fail("could not stage the reshaped shadow file");
                }
                if (slurp(ETC_SHADOW, before, sizeof(before)) < 0) {
                    kill(b, SIGKILL);
                    (void)waitpid(b, &status, 0);

                    return fail("cannot read back the reshaped shadow file");
                }
            }
            (void)write(b_in[1], "reshaped-pw\nreshaped-pw\n", 24);
            close(b_in[1]);
            waiting[0] = '\0';
            (void)read_until(b_err[0], "changed shape", waiting, sizeof(waiting), 20000);
            if (waitpid(b, &status, 0) != b || !WIFEXITED(status)) {
                return fail("the writer whose record changes shape did not exit normally");
            }
            close(b_err[0]);
            if (WEXITSTATUS(status) == 0) {
                return fail(which == 0 ? "a writer published a malformed aging field"
                                       : "a writer published an unterminated final record");
            }
            if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(before, after) != 0) {
                return fail("a refusal for a reshaped record still rewrote the shadow file");
            }
        }
        restore_databases();
    }

    return 0;
}

/**
 * What `passwd` does on a machine whose entropy cannot be trusted.
 *
 * It is `getrandom`'s answer that decides which side runs, never a CPUID bit:
 * the kernel marks its generator trusted only when a hardware source actually
 * delivered, so a CPU that advertises `RDRAND` and does not deliver has no
 * trusted entropy while one with `RDSEED` alone has it, and a probe reading the
 * feature bits would assert the wrong direction on both.
 *
 * The order is the point. The salt is minted after the old password is accepted
 * and before the new one is asked for, so a machine that cannot mint refuses
 * without first collecting a secret it cannot use. So on the untrusted side the
 * script stops after the old password, the run must fail, both the library's
 * line and the program's must appear, `New password:` must never be printed,
 * and every database must be byte-identical.
 *
 * This case runs on both sides of that answer, which is why it asks `getrandom`
 * first instead of being two probes.
 */
static int check_entropy(void)
{
    char before[4096];
    char after[4096];
    char report[512];
    char script[256];
    int status;

    restore_databases();

    if (slurp(ETC_SHADOW, before, sizeof(before)) < 0) {
        return fail("cannot read the shadow file the entropy case guards");
    }

    /* The script ends after the old password: if the mint came later, passwd
     * would prompt for a new password here and this feed would leave it
     * waiting, so the missing prompt is asserted, not assumed. */
    snprintf(script, sizeof(script), "%s\n", PROBE_PASS);
    status = run_passwd(PROBE_UID, NULL, script, report, sizeof(report));
    if (status == 0) {
        return fail("passwd changed a password with no trusted entropy");
    }
    if (strstr(report, "no trusted entropy") == NULL) {
        return fail("passwd refused for want of entropy without the library saying so");
    }
    if (strstr(report, "password unchanged") == NULL) {
        return fail("passwd refused for want of entropy without saying so itself");
    }
    if (strstr(report, "New password:") != NULL) {
        return fail("passwd asked for a password it could not have used");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(before, after) != 0) {
        return fail("a refusal for want of entropy altered the shadow file");
    }

    return 0;
}

/**
 * The refusals: a wrong password, another account, and an ambiguous database.
 * Each must leave every byte as it was.
 */
static int check_refusals(void)
{
    char before[4096];
    char after[4096];
    char line2[512];
    char hash2[256];
    char *made2;
    char report[512];
    char script[256];
    char doubled[4096];
    char line[512];
    int status;

    restore_databases();

    if (slurp(ETC_SHADOW, before, sizeof(before)) < 0) {
        return fail("cannot read the shadow file the refusal cases guard");
    }

    snprintf(script, sizeof(script), "not-the-password\nwhatever\nwhatever\n");
    status = run_passwd(PROBE_UID, NULL, script, report, sizeof(report));
    if (status == 0) {
        return fail("passwd accepted a wrong old password");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(before, after) != 0) {
        return fail("a refused change altered the shadow file");
    }

    /* The real-uid rule: this binary is 04755, so geteuid() is 0 for everyone
     * who runs it, and only getuid() says who asked. */
    snprintf(script, sizeof(script), "%s\nnope\nnope\n", PROBE_PASS);
    status = run_passwd(PROBE_UID, "root", script, report, sizeof(report));
    if (status == 0) {
        return fail("an unprivileged caller changed another account's password");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(before, after) != 0) {
        return fail("a refused cross-account change altered the shadow file");
    }

    /* An ambiguous database is refused, not resolved by file order. */
    if (line_of(before, PROBE_USER, line, sizeof(line)) != 0) {
        return fail("the shadow file does not name the probe account once");
    }
    if ((size_t)snprintf(doubled, sizeof(doubled), "%s%s\n", before, line) >= sizeof(doubled)) {
        return fail("the doubled shadow file does not fit");
    }
    if (spill(ETC_SHADOW, doubled, 0600) != 0) {
        return fail("could not stage the shadow file that names an account twice");
    }
    snprintf(script, sizeof(script), "%s\nnope\nnope\n", PROBE_PASS);
    status = run_passwd(PROBE_UID, NULL, script, report, sizeof(report));
    if (status == 0) {
        (void)spill(ETC_SHADOW, before, 0600);

        return fail("passwd changed an account its database names twice");
    }
    if (strstr(report, "more than once") == NULL) {
        (void)spill(ETC_SHADOW, before, 0600);

        return fail("passwd refused an ambiguous database without naming why");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(after, doubled) != 0) {
        (void)spill(ETC_SHADOW, before, 0600);

        return fail("a refusal for an ambiguous database still rewrote it");
    }
    if (spill(ETC_SHADOW, before, 0600) != 0) {
        return fail("could not put the shadow file back after the ambiguous case");
    }

    /* A record that does not parse is refused, not rewritten. A
     * replacement that kept a malformed line's shape would publish a file this
     * program calls changed and every reader goes on rejecting: `getspnam`
     * wants nine fields, so the account would answer to no password at all and
     * the user would be told the new one was set. */
    {
        char broken[4096];
        char rest[4096];

        without(before, PROBE_USER, rest, sizeof(rest));
        if ((size_t)snprintf(broken, sizeof(broken), "%s%s:%s:0:0:not-a-number:7:::\n", rest,
                             PROBE_USER, hash_from(before, PROBE_USER)) >= sizeof(broken)) {
            return fail("the malformed shadow file does not fit");
        }
        if (spill(ETC_SHADOW, broken, 0600) != 0) {
            return fail("could not stage the malformed shadow file");
        }
        snprintf(script, sizeof(script), "%s\nnope\nnope\n", PROBE_PASS);
        status = run_passwd(PROBE_UID, NULL, script, report, sizeof(report));
        if (status == 0) {
            (void)spill(ETC_SHADOW, before, 0600);

            return fail("passwd rewrote a record that is not a shadow record");
        }
        if (strstr(report, "not a shadow record") == NULL) {
            (void)spill(ETC_SHADOW, before, 0600);

            return fail("passwd refused a malformed record without naming why");
        }
        if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(after, broken) != 0) {
            (void)spill(ETC_SHADOW, before, 0600);

            return fail("a refusal for a malformed record still rewrote the file");
        }
        if (spill(ETC_SHADOW, before, 0600) != 0) {
            return fail("could not put the shadow file back after the malformed case");
        }
    }

    /* A record with no newline after it is one the reader refuses: musl's
     * getspnam rejects a matching line whose last character is not a newline,
     * so publishing it would leave an account answering to nothing while this
     * program reported the password changed. */
    {
        char unterminated[4096];
        char rest[4096];
        char record[512];

        without(before, PROBE_USER, rest, sizeof(rest));
        if (line_of(before, PROBE_USER, record, sizeof(record)) != 0) {
            return fail("the shadow file does not name the probe account once");
        }
        if ((size_t)snprintf(unterminated, sizeof(unterminated), "%s%s", rest, record) >=
            sizeof(unterminated)) {
            return fail("the unterminated shadow file does not fit");
        }
        if (spill(ETC_SHADOW, unterminated, 0600) != 0) {
            return fail("could not stage the unterminated shadow file");
        }
        snprintf(script, sizeof(script), "%s\nnope\nnope\n", PROBE_PASS);
        status = run_passwd(PROBE_UID, NULL, script, report, sizeof(report));
        if (status == 0) {
            (void)spill(ETC_SHADOW, before, 0600);

            return fail("passwd rewrote a record with no newline after it");
        }
        if (strstr(report, "no newline") == NULL) {
            (void)spill(ETC_SHADOW, before, 0600);

            return fail("passwd refused an unterminated record without naming why");
        }
        if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(after, unterminated) != 0) {
            (void)spill(ETC_SHADOW, before, 0600);

            return fail("a refusal for an unterminated record still rewrote the file");
        }
        if (spill(ETC_SHADOW, before, 0600) != 0) {
            return fail("could not put the shadow file back after the unterminated case");
        }
    }

    /* A caller may name their own account: it is the same request as naming
     * none, and it is how the command is conventionally written. */
    {
        char feed[256];

        snprintf(feed, sizeof(feed), "%s\nself-named-pw\nself-named-pw\n", PROBE_PASS);
        status = run_passwd(PROBE_UID, PROBE_USER, feed, report, sizeof(report));
        if (status != 0) {
            return fail("a caller could not name their own account");
        }
        if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 ||
            line_of(after, PROBE_USER, line2, sizeof(line2)) != 0 ||
            hash_of_line(line2, hash2, sizeof(hash2)) != 0) {
            return fail("cannot read back the self-named change");
        }
        made2 = crypt("self-named-pw", hash2);
        if (!made2 || strcmp(made2, hash2) != 0) {
            return fail("naming your own account did not change its password");
        }
        restore_databases();
        if (slurp(ETC_SHADOW, before, sizeof(before)) < 0) {
            return fail("cannot re-read the shadow file after the self-named case");
        }
    }

    /* A NUL is not a password byte: `strcmp` and `crypt` both stop there, so
     * two entries differing only past it would confirm each other and the
     * stored secret would be the prefix alone. */
    {
        char feed[256];
        size_t n = 0;

        n += (size_t)snprintf(feed + n, sizeof(feed) - n, "%s\n", PROBE_PASS);
        memcpy(feed + n, "abc", 3);
        n += 3;
        feed[n++] = '\0';
        memcpy(feed + n, "X\nabc", 5);
        n += 5;
        feed[n++] = '\0';
        memcpy(feed + n, "Y\n", 2);
        n += 2;
        status = run_passwd_raw(PROBE_UID, NULL, feed, n, report, sizeof(report));
        if (status == 0) {
            return fail("passwd accepted two entries that differ only past a NUL");
        }
        if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(before, after) != 0) {
            return fail("a refusal for a NUL in the entry altered the shadow file");
        }
    }

    /* A passwd record musl's parser skips is not an account, whatever this
     * program can make of it: authenticating against one and rewriting its
     * shadow entry would report a change for a name no reader resolves. */
    {
        static const char *const broken[] = {"%s:x:%djunk:%d:probe:/home/probe:/idreport\n",
                                             "%s:x:%d:%d:probe\n",
                                             "%s:x:%d:%djunk:probe:/home/probe:/idreport\n",
                                             "%s:x::%d:probe:/home/probe:/idreport\n"};
        size_t which;

        for (which = 0; which < sizeof(broken) / sizeof(broken[0]); which++) {
            char staged[4096];
            char feed[256];
            size_t used;

            restore_databases();
            if (slurp(ETC_PASSWD, staged, sizeof(staged)) < 0) {
                return fail("cannot read the passwd file the malformed-record case rewrites");
            }
            if (strip_line(staged, PROBE_USER) != 0) {
                return fail("cannot take the probe account out of the passwd file");
            }
            used = strlen(staged);
            if ((size_t)snprintf(staged + used, sizeof(staged) - used, broken[which], PROBE_USER,
                                 PROBE_UID, PROBE_UID) >= sizeof(staged) - used) {
                return fail("the malformed passwd record does not fit");
            }
            if (spill(ETC_PASSWD, staged, 0644) != 0) {
                return fail("could not stage the malformed passwd record");
            }
            snprintf(feed, sizeof(feed), "%s\nbroken-pw\nbroken-pw\n", PROBE_PASS);
            status = run_passwd(PROBE_UID, NULL, feed, report, sizeof(report));
            if (status == 0) {
                return fail("passwd acted on a record musl's parser skips");
            }
            if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(before, after) != 0) {
                return fail("a refusal for a malformed passwd record altered the shadow file");
            }
        }
        restore_databases();
    }

    /* A password longer than the shortest limit any reader in the image
     * imposes is refused, not stored as a prefix nobody typed. */
    {
        char longpw[256];
        size_t i;

        for (i = 0; i < sizeof(longpw) - 1; i++) {
            longpw[i] = 'a';
        }
        longpw[sizeof(longpw) - 1] = '\0';
        if ((size_t)snprintf(script, sizeof(script), "%s\n", PROBE_PASS) >= sizeof(script)) {
            return fail("the long-password script does not fit");
        }
        {
            char feed[1024];

            snprintf(feed, sizeof(feed), "%s\n%s\n%s\n", PROBE_PASS, longpw, longpw);
            status = run_passwd(PROBE_UID, NULL, feed, report, sizeof(report));
        }
        if (status == 0) {
            return fail("passwd stored a password longer than a reader can accept");
        }
        if (strstr(report, "longer than") == NULL) {
            return fail("passwd refused an overlong password without naming why");
        }
        if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(before, after) != 0) {
            return fail("a refusal for an overlong password altered the shadow file");
        }
    }

    return 0;
}

int main(void)
{
    unsigned char sample[16];
    ssize_t drawn;
    size_t i;
    int trusted = 0;
    int rc;

    /* A refused change exits before it has read the rest of the script, so the
     * write that feeds it finds a pipe with no reader. That is the child doing
     * exactly what it should, and it must not take this probe down with it. */
    signal(SIGPIPE, SIG_IGN);

    for (i = 0; i < 3; i++) {
        Snapshot_len[i] = slurp(Databases[i], Snapshot[i], sizeof(Snapshot[i]));
        if (Snapshot_len[i] < 0) {
            printf("PASSWDMUSL FAIL (cannot read %s)\n", Databases[i]);

            return 1;
        }
    }

    /* Which side runs is `getrandom`'s answer and never a feature bit: the
     * kernel marks its generator trusted only when a hardware source actually
     * delivered, so a CPU that advertises `RDRAND` without delivering has no
     * trusted entropy and one with `RDSEED` alone has it. */
    drawn = getrandom(sample, sizeof(sample), 0);
    if (drawn == (ssize_t)sizeof(sample)) {
        rc = check_change();
        if (rc == 0) {
            rc = check_refusals();
        }
        if (rc == 0) {
            rc = check_concurrent();
        }
        trusted = 1;
    } else if (drawn < 0 && errno == EIO) {
        rc = check_entropy();
    } else {
        rc = fail("getrandom answered neither a full draw nor the fail-closed -EIO");
    }

    /* The probes share one boot, and the ones after this read these files. */
    for (i = 0; i < 3; i++) {
        (void)spill(Databases[i], Snapshot[i], i == 1 ? 0600 : 0644);
    }
    (void)unlink(CRED_LOCK_PATH);

    if (rc != 0) {
        return rc;
    }
    /* Two gates read this, and neither can be satisfied by the side the other
     * expects. The suite scores on the kernel's exit line, so the status is 0
     * only on the trusted side and a fail-closed boot under `-cpu max` fails
     * it. `tests/run noentropy` matches a token and never reads the status, so
     * the fail-closed side is named and exits non-zero. */
    if (!trusted) {
        printf("PASSWDMUSL OK (fail-closed)\n");

        return 1;
    }
    printf("PASSWDMUSL OK\n");

    return 0;
}
