/**
 * addusermusl: check `adduser`, which makes an account across three databases
 * and a home directory.
 *
 * link-deps: utils/pwauth.c
 *
 * What `passwdmusl` shows for one file this shows for four things published in
 * order, and the order is the whole subject: nothing here makes the four one
 * change, so what makes an interrupted run safe is that every prefix of the
 * order is a system a reader can still make sense of. The cases, as they run:
 *
 * The end-to-end case, checked through libc as well as on the bytes:
 * `getpwnam` and `getgrnam` must parse the new account back with one number for
 * its uid and its gid, the shadow field must be a `$6$` hash `crypt` reproduces
 * from the scripted password, the home must be 0700 owned by those numbers, and
 * every line that was there before must come through byte for byte.
 *
 * The failed-password case, which shows what the order gains: a new account
 * whose two password lines disagree, after which the account still exists in
 * all three files with its field still `!`, so it cannot be entered and there
 * is nothing to roll back.
 *
 * The refusals, each followed by all three files byte-identical and no lock
 * left behind, including the residue no crash of `adduser` can produce: a name
 * written by hand into `/etc/group` alone, which a check of each file for the
 * name catches and a check on the account as a whole would not.
 *
 * The injection, one round per publication point, run through the crash build
 * `/adduser-crash N`, which exits with status 9 as soon as that publication
 * is made. It exits holding the transaction, so every round leaves the lock
 * behind: the probe checks it is there, removes it as root would, and only
 * then runs the shipped `adduser`, which would otherwise spend its ten
 * retries on the lock, since no caller breaks one, and not reach the residue
 * the round is about. Each round checks what must be present, what must still
 * be byte-identical, and that the re-run refuses, naming the files that have
 * the leftovers.
 *
 * Then contention. `passwd` takes its transaction after its prompts, so no live
 * writer here holds the lock across anything a test can synchronize on, and the
 * deterministic half is built on the lock a crashed writer leaves: against it
 * `adduser` must say it is waiting, be alive, and have published nothing,
 * which is the pair an `adduser` that ignored the lock fails. The live half
 * races two writers, a password change against a creation, and then two
 * creations, which must not agree on a number.
 *
 * The closing cross-check runs over the final files in both directions: every
 * passwd line has a shadow line and a group line whose gid it names, and no
 * name appears twice anywhere. An `adduser` that ignored the lock leaves the
 * first failure and one that checked two databases leaves the second.
 *
 * Prints "ADDUSERMUSL OK" on success and "ADDUSERMUSL FAIL (detail)" otherwise.
 * It restores all three databases byte for byte and removes what it created
 * before it exits, since the probes share one boot.
 */

#define _GNU_SOURCE /* crypt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pwauth.h"

#define ADDUSER_PATH "/adduser"
#define CRASH_PATH   "/adduser-crash"
#define PASSWD_PATH  "/passwd"
#define ETC_PASSWD   "/etc/passwd"
#define ETC_SHADOW   "/etc/shadow"
#define ETC_GROUP    "/etc/group"

/* The names this probe creates, all removed before it exits. */
#define NEW_USER   "newbie"
#define NEW_PASS   "newbiepass"
#define HALF_USER  "halfway"
#define CRASH_USER "crashed"

/* The staged account the concurrent case changes the password of. */
#define PROBE_USER "probe"
#define PROBE_PASS "probepass"
#define PROBE_UID  1000

/* The three databases, and their contents as `main` found them, which
 * every case compares against and `restore_databases` writes back. */
static const char *Databases[3] = {ETC_PASSWD, ETC_SHADOW, ETC_GROUP};
static char Snapshot[3][4096];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("ADDUSERMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Read up to `size - 1` bytes of `path` into `buffer`, terminated. Returns the
 * length, or -1.
 */
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

/** Write `text` to `path` at `mode`, creating or truncating it; 0, or -1. */
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

/** Whether `path` exists. */
static int present(const char *path)
{
    struct stat info;

    return stat(path, &info) == 0;
}

/** Whether a line of `text` begins with `name` and a colon. */
static int names_line(const char *text, const char *name)
{
    size_t len = strlen(name);
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, name, len) == 0 && line[len] == ':') {
            return 1;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return 0;
}

/** How many lines of `text` begin with `name` and a colon. */
static int count_lines(const char *text, const char *name)
{
    size_t len = strlen(name);
    const char *line = text;
    int found = 0;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, name, len) == 0 && line[len] == ':') {
            found++;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return found;
}

/**
 * Copy field `which` (counting from zero) of `name`'s line in `text` into
 * `out`. Returns 0 when the line was there and the field fit.
 */
static int field_of(const char *text, const char *name, int which, char *out, size_t size)
{
    size_t len = strlen(name);
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, name, len) == 0 && line[len] == ':') {
            const char *field = line;
            const char *stop;
            size_t width;
            int n = 0;

            while (n < which) {
                stop = memchr(field, ':', span - (size_t)(field - line));
                if (!stop) {
                    return -1;
                }
                field = stop + 1;
                n++;
            }
            stop = memchr(field, ':', span - (size_t)(field - line));
            width = stop ? (size_t)(stop - field) : span - (size_t)(field - line);
            if (width >= size) {
                return -1;
            }
            memcpy(out, field, width);
            out[width] = '\0';

            return 0;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return -1;
}

/** Read the three databases into `Snapshot`; 0, or -1. */
static int snapshot_databases(void)
{
    size_t i;

    for (i = 0; i < 3; i++) {
        if (slurp(Databases[i], Snapshot[i], sizeof(Snapshot[i])) < 0) {
            return -1;
        }
    }

    return 0;
}

/**
 * Write the snapshots back to the databases, and remove the homes and the
 * lock a case may have left. Failures are ignored.
 */
static void restore_databases(void)
{
    static const char *const homes[] = {"/home/" NEW_USER, "/home/" HALF_USER, "/home/" CRASH_USER};
    size_t i;

    for (i = 0; i < 3; i++) {
        (void)spill(Databases[i], Snapshot[i], i == 1 ? 0600 : 0644);
    }
    for (i = 0; i < sizeof(homes) / sizeof(homes[0]); i++) {
        (void)rmdir(homes[i]);
    }
    (void)unlink(CRED_LOCK_PATH);
}

/**
 * Read from `fd`, appending to the terminated string in `buffer` of `size`
 * bytes, until `needle` appears in it. Returns 1 on a match, and 0 at end of
 * file, on a read error, when the buffer is full, or once `ms` milliseconds
 * of polls have passed with nothing to read.
 *
 * The waiting case uses this in place of a sleep: the needle is a line the
 * other process could print only after doing what is waited for.
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
 * Read from `fd` to end of file, appending to the terminated string in
 * `buffer` of `size` bytes, until the buffer is full or `ms` milliseconds of
 * polls have passed with nothing to read.
 *
 * It reads all that a child writes: a reader that stopped at the first chunk
 * would leave the pipe to be closed under a child still writing, which would
 * get `SIGPIPE`, and the probe would fail for a fault of its own. End of file
 * comes when the child exits, so this is also how the parent knows the child
 * has finished writing.
 */
static void drain(int fd, char *buffer, size_t size, long ms)
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
                return;
            }
            held += (size_t)got;
            buffer[held] = '\0';
            if (held + 1 >= size) {
                return;
            }
            continue;
        }
        waited += 200;
    }
}

/**
 * Run `path` with `argv`, feeding `script` to its standard input and collecting
 * its standard error into `report`. Returns the child's exit status, or -1
 * when it cannot be run or does not exit.
 *
 * The input is a pipe, which is what makes the script possible: the password
 * prompt of the `passwd` that `adduser` execs reads a pipe without changing
 * terminal modes and takes the lines as they come.
 */
static int run_with(const char *path, char *const argv[], const char *script, char *report,
                    size_t report_size)
{
    int in[2];
    int err[2];
    int status;
    pid_t child;

    report[0] = '\0';
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
        execv(path, argv);
        _exit(91);
    }
    close(in[0]);
    close(err[1]);
    if (child < 0) {
        close(in[1]);
        close(err[0]);

        return -1;
    }
    if (script && *script) {
        (void)write(in[1], script, strlen(script));
    }
    close(in[1]);
    drain(err[0], report, report_size, 30000);
    close(err[0]);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

/** As `run_with`, for the shipped `adduser` on `name`. */
static int run_adduser(const char *name, const char *script, char *report, size_t report_size)
{
    char *argv[3];

    argv[0] = (char *)"adduser";
    argv[1] = (char *)name;
    argv[2] = NULL;

    return run_with(ADDUSER_PATH, argv, script, report, report_size);
}

/**
 * Check an account made end to end, read back through libc. Returns 0, or 1
 * after printing the failure.
 *
 * The reads through `getpwnam`, `getgrnam` and `getspnam` show that the
 * readers can parse what was written, which the bytes alone do not show.
 * Every line that was there before must still be there, byte for byte, since
 * a check of the new account alone would pass on a file that lost every
 * other line.
 */
static int check_created(void)
{
    char report[1024];
    char after[3][4096];
    char field[256];
    struct passwd *pw;
    struct group *gr;
    struct spwd *sp;
    struct stat info;
    char *made;
    char script[128];
    size_t i;
    int status;

    restore_databases();
    snprintf(script, sizeof(script), "%s\n%s\n", NEW_PASS, NEW_PASS);
    status = run_adduser(NEW_USER, script, report, sizeof(report));
    if (status != 0) {
        return fail("adduser refused a new account");
    }
    pw = getpwnam(NEW_USER);
    if (!pw) {
        return fail("getpwnam cannot parse the account adduser wrote");
    }
    if (pw->pw_uid < 1000 || pw->pw_gid != pw->pw_uid) {
        return fail("the new account's uid and gid do not agree");
    }
    gr = getgrnam(NEW_USER);
    if (!gr || gr->gr_gid != pw->pw_gid) {
        return fail("getgrnam cannot parse the group adduser wrote");
    }
    sp = getspnam(NEW_USER);
    if (!sp || !sp->sp_pwdp) {
        return fail("getspnam cannot parse the shadow line adduser wrote");
    }
    if (strncmp(sp->sp_pwdp, "$6$", 3) != 0) {
        return fail("the new account's password is not a sha512 hash");
    }
    made = crypt(NEW_PASS, sp->sp_pwdp);
    if (!made || strcmp(made, sp->sp_pwdp) != 0) {
        return fail("the new account's hash is not of the password that was set");
    }
    if (stat(pw->pw_dir, &info) != 0) {
        return fail("the new account has no home directory");
    }
    if ((info.st_mode & 07777) != 0700) {
        return fail("the new home directory is not 0700");
    }
    if (info.st_uid != pw->pw_uid || info.st_gid != pw->pw_gid) {
        return fail("the new home directory is not owned by the new account");
    }
    for (i = 0; i < 3; i++) {
        const char *line = Snapshot[i];

        if (slurp(Databases[i], after[i], sizeof(after[i])) < 0) {
            return fail("cannot read back a database adduser rewrote");
        }
        while (*line) {
            const char *end = strchr(line, '\n');
            size_t span = end ? (size_t)(end - line) : strlen(line);
            char copy[512];

            if (span > 0 && span < sizeof(copy)) {
                memcpy(copy, line, span);
                copy[span] = '\0';
                if (strstr(after[i], copy) == NULL) {
                    return fail("a line that was there before adduser ran is gone");
                }
            }
            if (!end) {
                break;
            }
            line = end + 1;
        }
    }
    if (field_of(after[0], NEW_USER, 6, field, sizeof(field)) != 0 ||
        strcmp(field, "/bin/sh") != 0) {
        return fail("the new account did not get the default shell");
    }
    if (present(CRED_LOCK_PATH)) {
        return fail("adduser left its lock behind");
    }

    return 0;
}

/**
 * Check an account whose password is never set. Returns 0, or 1 after printing
 * the failure.
 *
 * `adduser` publishes the account with a locked field and hands it to `passwd`,
 * so a password that is never set leaves an account that exists and cannot be
 * entered. That is the end state of the last prefix, and nothing needs
 * undoing.
 */
static int check_failed_password(void)
{
    char report[1024];
    char after[4096];
    char field[256];
    int status;

    restore_databases();
    status = run_adduser(HALF_USER, "one-password\nanother-password\n", report, sizeof(report));
    if (status == 0) {
        return fail("the exec'd passwd accepted two different new passwords");
    }
    if (!getpwnam(HALF_USER) || !getgrnam(HALF_USER)) {
        return fail("the account with no password is not in passwd and group");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 ||
        field_of(after, HALF_USER, 1, field, sizeof(field)) != 0) {
        return fail("the account with no password has no shadow line");
    }
    if (strcmp(field, "!") != 0) {
        return fail("the account whose password was never set is not locked");
    }
    if (present(CRED_LOCK_PATH)) {
        return fail("adduser left its lock behind after the password failed");
    }

    return 0;
}

/**
 * Check the refusals, each of which must change nothing: an account that
 * exists, uid 0, a uid already taken, a caller who is not root, and a name
 * written by hand into the group file alone, which a check on the account as a
 * whole would call absent. Returns 0, or 1 after printing the failure.
 */
static int check_refusals(void)
{
    char report[1024];
    char before[3][4096];
    char after[4096];
    char staged[4096];
    char *argv[6];
    size_t used;
    size_t i;
    int status;

    restore_databases();
    for (i = 0; i < 3; i++) {
        if (slurp(Databases[i], before[i], sizeof(before[i])) < 0) {
            return fail("cannot read a database before the refusals");
        }
    }

    /* An account that already exists. */
    status = run_adduser(PROBE_USER, "", report, sizeof(report));
    if (status == 0) {
        return fail("adduser created an account that already existed");
    }
    if (strstr(report, "already exists") == NULL) {
        return fail("adduser did not report an existing account as one");
    }

    /* uid 0. */
    argv[0] = (char *)"adduser";
    argv[1] = (char *)"-u";
    argv[2] = (char *)"0";
    argv[3] = (char *)NEW_USER;
    argv[4] = NULL;
    if (run_with(ADDUSER_PATH, argv, "", report, sizeof(report)) == 0) {
        return fail("adduser handed out uid 0");
    }

    /* A uid already taken. */
    argv[2] = (char *)"1000";
    if (run_with(ADDUSER_PATH, argv, "", report, sizeof(report)) == 0) {
        return fail("adduser handed out a uid that was taken");
    }
    if (strstr(report, ETC_PASSWD) == NULL) {
        return fail("adduser did not name the file holding the taken uid");
    }

    /* A caller who is not root, refused before anything is opened. */
    {
        pid_t child = fork();
        int wait_status;

        if (child == 0) {
            if (setuid(PROBE_UID) != 0) {
                _exit(90);
            }
            execl(ADDUSER_PATH, "adduser", NEW_USER, (char *)0);
            _exit(91);
        }
        if (child < 0 || waitpid(child, &wait_status, 0) != child || !WIFEXITED(wait_status)) {
            return fail("could not run adduser as an unprivileged caller");
        }
        if (WEXITSTATUS(wait_status) == 0) {
            return fail("an unprivileged caller created an account");
        }
    }

    /* A name written by hand into the group file alone. */
    if (slurp(ETC_GROUP, staged, sizeof(staged)) < 0) {
        return fail("cannot read the group file the residue case writes");
    }
    used = strlen(staged);
    if ((size_t)snprintf(staged + used, sizeof(staged) - used, "%s:x:1234:\n", NEW_USER) >=
        sizeof(staged) - used) {
        return fail("the group file with a leftover line does not fit");
    }
    if (spill(ETC_GROUP, staged, 0644) != 0) {
        return fail("could not stage the leftover group line");
    }
    status = run_adduser(NEW_USER, "", report, sizeof(report));
    if (status == 0) {
        return fail("adduser created an account over a leftover group line");
    }
    if (strstr(report, ETC_GROUP) == NULL) {
        return fail("adduser did not name the file holding the leftover line");
    }
    if (slurp(ETC_PASSWD, after, sizeof(after)) < 0 || strcmp(after, before[0]) != 0) {
        return fail("a refused adduser still rewrote the passwd file");
    }
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 || strcmp(after, before[1]) != 0) {
        return fail("a refused adduser still rewrote the shadow file");
    }
    restore_databases();
    for (i = 0; i < 3; i++) {
        if (slurp(Databases[i], after, sizeof(after)) < 0 || strcmp(after, before[i]) != 0) {
            return fail("a database did not come back to what it was");
        }
    }
    if (present(CRED_LOCK_PATH)) {
        return fail("a refused adduser left a lock behind");
    }

    return 0;
}

/**
 * Check the publication order by injecting a crash at each point. Returns 0,
 * or 1 after printing the failure.
 *
 * One round per publication point, each run through the crash build, which
 * exits as soon as that publication is made and inside the transaction, so it
 * leaves the lock a crash leaves. The probe removes that lock as root would
 * before running the shipped program, since no caller breaks a lock and the
 * re-run would otherwise fail on the lock and not on the residue.
 *
 * Shadow comes first, with a locked field: the prefix that uses no number and
 * names no group. A file not yet published must be byte-identical, and not
 * merely lack the name, since a rewrite that lost other lines would pass that.
 * The re-run must refuse, naming the files that have the leftovers, except
 * after the last point, where the account is complete and `passwd` can finish
 * it.
 */
static int check_injection(void)
{
    static const char *const wanted[] = {ETC_SHADOW, ETC_GROUP, "/home/" CRASH_USER, ETC_PASSWD};
    char report[1024];
    char after[4096];
    char field[256];
    char point[8];
    char *argv[4];
    int which;

    for (which = 1; which <= 4; which++) {
        restore_databases();
        snprintf(point, sizeof(point), "%d", which);
        argv[0] = (char *)"adduser-crash";
        argv[1] = point;
        argv[2] = (char *)CRASH_USER;
        argv[3] = NULL;
        if (run_with(CRASH_PATH, argv, "", report, sizeof(report)) != 9) {
            return fail("the crash build did not die where it was told to");
        }
        /* It died inside the transaction, so the lock is the residue too. */
        if (!present(CRED_LOCK_PATH)) {
            return fail("a crash inside the transaction left no lock behind");
        }
        (void)unlink(CRED_LOCK_PATH);

        /* Shadow first, with a locked field. */
        if (slurp(ETC_SHADOW, after, sizeof(after)) < 0) {
            return fail("cannot read the shadow file after the crash");
        }
        if (count_lines(after, CRASH_USER) != 1) {
            return fail("the crash did not leave exactly one shadow line");
        }
        if (field_of(after, CRASH_USER, 1, field, sizeof(field)) != 0 || strcmp(field, "!") != 0) {
            return fail("the shadow line the crash left is not locked");
        }
        if (!getspnam(CRASH_USER)) {
            return fail("getspnam cannot find the line the crash left");
        }

        if (slurp(ETC_GROUP, after, sizeof(after)) < 0) {
            return fail("cannot read the group file after the crash");
        }
        if (names_line(after, CRASH_USER) != (which >= 2)) {
            return fail("the group file does not match the publication point");
        }
        if (present("/home/" CRASH_USER) != (which >= 3)) {
            return fail("the home directory does not match the publication point");
        }
        if (slurp(ETC_PASSWD, after, sizeof(after)) < 0) {
            return fail("cannot read the passwd file after the crash");
        }
        if (names_line(after, CRASH_USER) != (which >= 4)) {
            return fail("the passwd file does not match the publication point");
        }
        /* What has not been published is byte-identical. */
        if (which < 2 &&
            (slurp(ETC_GROUP, after, sizeof(after)) < 0 || strcmp(after, Snapshot[2]) != 0)) {
            return fail("a file published after the crash point was changed anyway");
        }
        if (which < 4 &&
            (slurp(ETC_PASSWD, after, sizeof(after)) < 0 || strcmp(after, Snapshot[0]) != 0)) {
            return fail("the passwd file was changed before its turn");
        }

        /* The re-run. */
        if (which == 4) {
            char script[128];

            if (run_adduser(CRASH_USER, "", report, sizeof(report)) == 0) {
                return fail("adduser created an account that was already complete");
            }
            if (strstr(report, "already exists") == NULL) {
                return fail("a complete account was reported as a residue");
            }
            /* The last prefix can be finished. */
            snprintf(script, sizeof(script), "%s\n%s\n", NEW_PASS, NEW_PASS);
            argv[0] = (char *)"passwd";
            argv[1] = (char *)CRASH_USER;
            argv[2] = NULL;
            if (run_with(PASSWD_PATH, argv, script, report, sizeof(report)) != 0) {
                return fail("passwd could not finish the account the crash completed");
            }
            if (slurp(ETC_SHADOW, after, sizeof(after)) < 0 ||
                field_of(after, CRASH_USER, 1, field, sizeof(field)) != 0 ||
                strncmp(field, "$6$", 3) != 0) {
                return fail("the finished account still has no password");
            }
        } else {
            if (run_adduser(CRASH_USER, "", report, sizeof(report)) == 0) {
                return fail("adduser created an account over a crash residue");
            }
            if (strstr(report, wanted[0]) == NULL) {
                return fail("the refusal did not name the shadow file");
            }
            if (which >= 2 && strstr(report, ETC_GROUP) == NULL) {
                return fail("the refusal did not name the group file");
            }
            if (slurp(ETC_SHADOW, after, sizeof(after)) < 0) {
                return fail("cannot read the shadow file after the refusal");
            }
            if (count_lines(after, CRASH_USER) != 1) {
                return fail("the refused re-run appended a second shadow line");
            }
        }
    }
    restore_databases();

    return 0;
}

/**
 * Check that `adduser` waits for a held lock. Returns 0, or 1 after printing
 * the failure.
 *
 * `passwd` takes its transaction after its prompts, so a writer held at a
 * prompt holds no lock, and no live writer here keeps the lock across
 * anything a test can synchronize on. So this case is built on the lock a
 * crashed writer leaves. The crash build exits inside its transaction, and
 * against that lock `adduser` must say it is waiting, must still be alive,
 * and must have published nothing: an `adduser` that ignored the lock would
 * have read the snapshot and published all four replacements in this time.
 * The lock is then removed as root would remove it, since the library never
 * breaks one, and the run must create the account: the crash left `crashed`
 * in the shadow file, and this run is for a different name.
 */
static int check_waits(void)
{
    char report[1024];
    char after[4096];
    char before[3][4096];
    char *argv[4];
    int a_in[2];
    int a_err[2];
    pid_t maker;
    int status;
    size_t i;

    restore_databases();
    argv[0] = (char *)"adduser-crash";
    argv[1] = (char *)"1";
    argv[2] = (char *)CRASH_USER;
    argv[3] = NULL;
    if (run_with(CRASH_PATH, argv, "", report, sizeof(report)) != 9) {
        return fail("the crash build did not leave a lock to wait on");
    }
    if (!present(CRED_LOCK_PATH)) {
        return fail("the crashed writer left no lock");
    }
    for (i = 0; i < 3; i++) {
        if (slurp(Databases[i], before[i], sizeof(before[i])) < 0) {
            return fail("cannot read a database before the waiting case");
        }
    }
    if (pipe(a_in) != 0 || pipe(a_err) != 0) {
        return fail("could not make the pipes the waiting maker reports on");
    }
    maker = fork();
    if (maker == 0) {
        close(a_in[1]);
        close(a_err[0]);
        if (dup2(a_in[0], 0) < 0 || dup2(a_err[1], 2) < 0) {
            _exit(90);
        }
        execl(ADDUSER_PATH, "adduser", NEW_USER, (char *)0);
        _exit(91);
    }
    close(a_in[0]);
    close(a_err[1]);
    if (maker < 0) {
        return fail("could not fork the waiting maker");
    }
    report[0] = '\0';
    if (!read_until(a_err[0], "busy, waiting", report, sizeof(report), 20000)) {
        kill(maker, SIGKILL);
        (void)waitpid(maker, &status, 0);

        return fail("the account maker did not report the lock as held");
    }
    if (waitpid(maker, &status, WNOHANG) != 0) {
        return fail("the account maker did not wait for the lock");
    }
    for (i = 0; i < 3; i++) {
        if (slurp(Databases[i], after, sizeof(after)) < 0 || strcmp(after, before[i]) != 0) {
            kill(maker, SIGKILL);
            (void)waitpid(maker, &status, 0);

            return fail("the account maker published while the lock was held");
        }
    }
    (void)unlink(CRED_LOCK_PATH);
    (void)write(a_in[1], NEW_PASS "\n" NEW_PASS "\n", 2 * strlen(NEW_PASS) + 2);
    close(a_in[1]);
    if (waitpid(maker, &status, 0) != maker || !WIFEXITED(status)) {
        return fail("the waiting maker did not exit normally");
    }
    close(a_err[0]);
    if (WEXITSTATUS(status) != 0) {
        return fail("the maker failed after the lock was freed");
    }
    if (!getpwnam(NEW_USER)) {
        return fail("the account was not created once the lock was freed");
    }
    if (present(CRED_LOCK_PATH)) {
        return fail("the waiting case left a lock behind");
    }
    restore_databases();

    return 0;
}

/**
 * Check two writers at once, and then two account makers at once. Returns 0,
 * or 1 after printing the failure.
 *
 * Nothing here is staged: both pairs are started together and race. What is
 * checked is what a lost update destroys, that both results survive, since
 * each writer must build its replacement from a file that already has the
 * other's line, and what a number chosen outside the lock destroys, that two
 * accounts made at once have two different uids.
 *
 * Each of the two makers gets its own input pipe: two exec'd `passwd`
 * children sharing one pipe would race for lines a byte at a time, and the
 * probe would fail for a fault of its own. Both makers must succeed, and both
 * accounts must have a hash: each publishes its passwd line before it execs
 * `passwd`, so a run whose password never got set still leaves the name in
 * every file, with a `!` field, and the uid checks would pass over two
 * half-made accounts.
 */
static int check_concurrent(void)
{
    char report[1024];
    char waiting[1024];
    char after[4096];
    char field[256];
    char script[128];
    int p_in[2];
    int p_err[2];
    int a_in[2];
    int a_err[2];
    pid_t writer;
    pid_t maker;
    int status;
    char *made;
    unsigned long first_uid;
    unsigned long second_uid;

    restore_databases();

    /* A password change and an account creation, racing. */
    if (pipe(p_in) != 0 || pipe(p_err) != 0 || pipe(a_in) != 0 || pipe(a_err) != 0) {
        return fail("could not make the pipes the racing writers report on");
    }
    writer = fork();
    if (writer == 0) {
        close(p_in[1]);
        close(p_err[0]);
        if (dup2(p_in[0], 0) < 0 || dup2(p_err[1], 2) < 0) {
            _exit(90);
        }
        if (setuid(PROBE_UID) != 0) {
            _exit(91);
        }
        execl(PASSWD_PATH, "passwd", (char *)0);
        _exit(92);
    }
    maker = fork();
    if (maker == 0) {
        close(a_in[1]);
        close(a_err[0]);
        if (dup2(a_in[0], 0) < 0 || dup2(a_err[1], 2) < 0) {
            _exit(90);
        }
        execl(ADDUSER_PATH, "adduser", NEW_USER, (char *)0);
        _exit(91);
    }
    close(p_in[0]);
    close(p_err[1]);
    close(a_in[0]);
    close(a_err[1]);
    if (writer < 0 || maker < 0) {
        return fail("could not fork the racing writers");
    }
    snprintf(script, sizeof(script), "%s\nconcurrent-pw\nconcurrent-pw\n", PROBE_PASS);
    (void)write(p_in[1], script, strlen(script));
    close(p_in[1]);
    (void)write(a_in[1], NEW_PASS "\n" NEW_PASS "\n", 2 * strlen(NEW_PASS) + 2);
    close(a_in[1]);
    waiting[0] = '\0';
    drain(p_err[0], waiting, sizeof(waiting), 30000);
    report[0] = '\0';
    drain(a_err[0], report, sizeof(report), 30000);
    close(p_err[0]);
    close(a_err[0]);
    if (waitpid(writer, &status, 0) != writer || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        (void)waitpid(maker, &status, 0);

        return fail("the racing password change failed");
    }
    if (waitpid(maker, &status, 0) != maker || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the racing account creation failed");
    }
    /* Both survive. */
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0) {
        return fail("cannot read the shadow file after the race");
    }
    if (field_of(after, PROBE_USER, 1, field, sizeof(field)) != 0) {
        return fail("the racing password change lost its shadow line");
    }
    made = crypt("concurrent-pw", field);
    if (!made || strcmp(made, field) != 0) {
        return fail("the racing password change was lost");
    }
    if (field_of(after, NEW_USER, 1, field, sizeof(field)) != 0 || strncmp(field, "$6$", 3) != 0) {
        return fail("the racing account creation was lost");
    }
    if (present(CRED_LOCK_PATH)) {
        return fail("the race left a lock behind");
    }

    /* Two account makers at once, which must not agree on a number. */
    restore_databases();
    {
        static const char *const names[2] = {NEW_USER, HALF_USER};
        int feed[2][2];
        int errs[2][2];
        pid_t makers[2];
        size_t which;

        for (which = 0; which < 2; which++) {
            if (pipe(feed[which]) != 0 || pipe(errs[which]) != 0) {
                return fail("could not make the pipes the two makers report on");
            }
        }
        for (which = 0; which < 2; which++) {
            makers[which] = fork();
            if (makers[which] == 0) {
                close(feed[which][1]);
                close(errs[which][0]);
                if (dup2(feed[which][0], 0) < 0 || dup2(errs[which][1], 2) < 0) {
                    _exit(90);
                }
                execl(ADDUSER_PATH, "adduser", names[which], (char *)0);
                _exit(91);
            }
        }
        for (which = 0; which < 2; which++) {
            close(feed[which][0]);
            close(errs[which][1]);
            if (makers[which] < 0) {
                return fail("could not fork the two account makers");
            }
        }
        for (which = 0; which < 2; which++) {
            (void)write(feed[which][1], NEW_PASS "\n" NEW_PASS "\n", 2 * strlen(NEW_PASS) + 2);
            close(feed[which][1]);
        }
        for (which = 0; which < 2; which++) {
            report[0] = '\0';
            drain(errs[which][0], report, sizeof(report), 30000);
            close(errs[which][0]);
            if (waitpid(makers[which], &status, 0) != makers[which] || !WIFEXITED(status)) {
                return fail("one of two concurrent makers did not exit normally");
            }
            if (WEXITSTATUS(status) != 0) {
                return fail("one of two concurrent makers failed");
            }
        }
    }
    if (slurp(ETC_PASSWD, after, sizeof(after)) < 0) {
        return fail("cannot read the passwd file after the two makers");
    }
    if (field_of(after, NEW_USER, 2, field, sizeof(field)) != 0) {
        return fail("the first of two concurrent accounts is not there");
    }
    first_uid = strtoul(field, NULL, 10);
    if (field_of(after, HALF_USER, 2, field, sizeof(field)) != 0) {
        return fail("the second of two concurrent accounts is not there");
    }
    second_uid = strtoul(field, NULL, 10);
    if (first_uid == second_uid) {
        return fail("two concurrent runs handed out the same uid");
    }
    if (count_lines(after, NEW_USER) != 1 || count_lines(after, HALF_USER) != 1) {
        return fail("a concurrent run appended a name twice");
    }
    /* Both have a password. */
    if (slurp(ETC_SHADOW, after, sizeof(after)) < 0) {
        return fail("cannot read the shadow file after the two makers");
    }
    if (field_of(after, NEW_USER, 1, field, sizeof(field)) != 0 || strncmp(field, "$6$", 3) != 0 ||
        field_of(after, HALF_USER, 1, field, sizeof(field)) != 0 || strncmp(field, "$6$", 3) != 0) {
        return fail("a concurrently created account has no password of its own");
    }
    if (present(CRED_LOCK_PATH)) {
        return fail("the two makers left a lock behind");
    }

    return 0;
}

/**
 * Check the databases the run left: every passwd line has a shadow line and a
 * group for its gid, and no name appears twice in any file. Returns 0, or 1
 * after printing the failure.
 *
 * An `adduser` that ignored the lock leaves the first failure, an `x` field
 * with no shadow line; one that checked only two databases leaves the second,
 * a name appearing twice.
 */
static int check_consistent(void)
{
    char passwd_text[4096];
    char group_text[4096];
    char shadow_text[4096];
    const char *line;

    if (slurp(ETC_PASSWD, passwd_text, sizeof(passwd_text)) < 0 ||
        slurp(ETC_GROUP, group_text, sizeof(group_text)) < 0 ||
        slurp(ETC_SHADOW, shadow_text, sizeof(shadow_text)) < 0) {
        return fail("cannot read the databases for the closing check");
    }
    line = passwd_text;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);
        const char *colon = memchr(line, ':', span);
        char name[256];
        char gid[64];
        struct group *gr;

        if (colon && (size_t)(colon - line) < sizeof(name)) {
            memcpy(name, line, (size_t)(colon - line));
            name[colon - line] = '\0';
            if (!names_line(shadow_text, name)) {
                return fail("an account in passwd has no shadow line");
            }
            if (count_lines(passwd_text, name) != 1 || count_lines(shadow_text, name) != 1 ||
                count_lines(group_text, name) > 1) {
                return fail("a name appears twice in a database");
            }
            if (field_of(passwd_text, name, 3, gid, sizeof(gid)) != 0) {
                return fail("an account in passwd has no gid field");
            }
            gr = getgrgid((gid_t)strtoul(gid, NULL, 10));
            if (!gr) {
                return fail("an account names a gid no group has");
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }

    return 0;
}

/**
 * Take the snapshots, run the cases, put back what they changed and print the
 * result. Must run as root. Returns 0 when every case passes, 1 otherwise.
 */
int main(void)
{
    int rc;

    if (getuid() != 0) {
        printf("ADDUSERMUSL FAIL (the probe must run as root)\n");

        return 1;
    }
    if (snapshot_databases() != 0) {
        printf("ADDUSERMUSL FAIL (cannot read the databases)\n");

        return 1;
    }
    rc = check_created();
    if (rc == 0) {
        rc = check_failed_password();
    }
    if (rc == 0) {
        rc = check_refusals();
    }
    if (rc == 0) {
        rc = check_injection();
    }
    if (rc == 0) {
        rc = check_waits();
    }
    if (rc == 0) {
        rc = check_concurrent();
    }
    if (rc == 0) {
        rc = check_consistent();
    }
    restore_databases();
    if (rc == 0) {
        printf("ADDUSERMUSL OK\n");
    }

    return rc;
}
