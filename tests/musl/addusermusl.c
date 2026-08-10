/**
 * `adduser` in the guest: an account made across three databases and a home.
 *
 * link-deps: utils/pwauth.c
 *
 * What `passwd` proves for one file this proves for four things published in
 * order, and the order is the whole subject: nothing here makes the four one
 * change, so what makes an interrupted run safe is that every prefix of the
 * order is a system a reader can still make sense of. The cases, as they run:
 *
 * The end-to-end case, asserted through libc rather than on bytes alone:
 * `getpwnam` and `getgrnam` must parse the new account back with one number for
 * its uid and its gid, the shadow field must be a `$6$` hash `crypt` reproduces
 * from the scripted password, the home must be 0700 owned by those numbers, and
 * every line that was there before must come through byte for byte.
 *
 * The failed-password case, which is what proves the order bought something: a
 * new account whose two password lines disagree, after which the account still
 * exists in all three files with its field still `!`, so it cannot be entered
 * and there is nothing to roll back.
 *
 * The refusals, each followed by all three files byte-identical and no lock
 * left behind, including the residue no crash of ours can produce: a name
 * written by hand into `/etc/group` alone, which a per-file absence check
 * catches and a check on the account as a whole would not.
 *
 * The injection, one round per publication point, run through the crash build
 * `/adduser-crash N`, which dies the instant that publication has landed. It
 * dies holding the transaction, so every round leaves the lock behind: the
 * probe asserts it is there, removes it the way root would, and only then runs
 * the real `adduser`, which by the never-break rule would otherwise spend ten
 * tries refusing on the lock rather than on the residue the round is about.
 * Each round asserts what must be present, what must still be byte-identical,
 * and that the re-run refuses naming the files that hold the leftovers.
 *
 * Then contention. `passwd` takes its transaction after its prompts, so no live
 * writer here holds the lock across anything a test can synchronize on, and the
 * deterministic half is built on the lock a crashed writer really leaves:
 * against it `adduser` must say it is waiting, be alive, and have published
 * nothing, which is the pair an `adduser` that ignored the lock fails. The live
 * half races two real writers, a password change against a creation, and then
 * two creations, which must not agree on a number.
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

static const char *Databases[3] = {ETC_PASSWD, ETC_SHADOW, ETC_GROUP};
static char Snapshot[3][4096];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("ADDUSERMUSL FAIL (%s)\n", what);

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

/** Take the pre-image every case compares against. Returns 0. */
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

/** Put the databases back and remove what a case created. */
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
 * Read from `fd` until `needle` appears or `ms` elapse. Returns 1 on a match.
 *
 * The rendezvous the concurrent case uses in place of a sleep: a token the
 * other process could only have printed after doing the thing waited for.
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
 * Read from `fd` until it reaches end of input or `ms` elapse, appending to
 * `buffer`. Returns nothing.
 *
 * The whole of what a child said, and it must be the whole: a reader that
 * stopped at the first chunk would leave the pipe to be closed under a child
 * still writing, which delivers `SIGPIPE` and turns this probe into a test of
 * its own harness. End of input arrives when the child exits, so this is also
 * how the parent knows the child is done talking.
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
 * its standard error into `report`. Returns the child's exit status, or -1.
 *
 * The input is a pipe rather than a terminal, which is what makes the script
 * possible: the password prompt `adduser` execs takes the non-tty path and
 * reads the lines as they come.
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

/** Run the shipped `adduser` on `name`, with `script` for the password. */
static int run_adduser(const char *name, const char *script, char *report, size_t report_size)
{
    char *argv[3];

    argv[0] = (char *)"adduser";
    argv[1] = (char *)name;
    argv[2] = NULL;

    return run_with(ADDUSER_PATH, argv, script, report, report_size);
}

/**
 * The account made end to end, read back through libc.
 *
 * Asserting through `getpwnam` and `getgrnam` rather than on the bytes alone is
 * the point of this case: it proves the readers can parse what was written,
 * which is a different claim from the bytes having landed.
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
    /* Every line that was there before, byte for byte. A check through the new
     * account alone would pass on a file that lost every other one. */
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
 * The password that fails, which is what proves the order bought something.
 *
 * `adduser` publishes the account with a locked field and hands it to `passwd`,
 * so a password that is never set leaves an account that exists and cannot be
 * entered. That is the intended end state of the last prefix rather than a
 * residue, and nothing needs undoing.
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
 * The refusals, each of which must change nothing at all.
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

    /* An account that already exists is not this run's business. */
    status = run_adduser(PROBE_USER, "", report, sizeof(report));
    if (status == 0) {
        return fail("adduser created an account that already existed");
    }
    if (strstr(report, "already exists") == NULL) {
        return fail("adduser did not report an existing account as one");
    }

    /* Root's number is not one to hand out. */
    argv[0] = (char *)"adduser";
    argv[1] = (char *)"-u";
    argv[2] = (char *)"0";
    argv[3] = (char *)NEW_USER;
    argv[4] = NULL;
    if (run_with(ADDUSER_PATH, argv, "", report, sizeof(report)) == 0) {
        return fail("adduser handed out uid 0");
    }

    /* A number already taken, as a uid and then as a gid. */
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

    /* The residue no crash of ours can produce: a name written by hand into one
     * database. A check on the account as a whole would call this absent. */
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
 * The ordering, proved by injection rather than by a reading of the source.
 *
 * One round per publication point, each run through the crash build, which dies
 * the instant that publication has landed and inside the transaction, so it
 * leaves the lock a real crash leaves. The probe clears that lock the way root
 * would before running the shipped program, since the never-break rule would
 * otherwise make the re-run refuse on the lock rather than on the residue.
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

        /* Shadow first, and with a locked field: the prefix that consumes no
         * number and names no group. */
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
        /* What has not been published must be byte-identical, not merely free
         * of the name: a rewrite that lost other lines would pass that. */
        if (which < 2 &&
            (slurp(ETC_GROUP, after, sizeof(after)) < 0 || strcmp(after, Snapshot[2]) != 0)) {
            return fail("a file published after the crash point was changed anyway");
        }
        if (which < 4 &&
            (slurp(ETC_PASSWD, after, sizeof(after)) < 0 || strcmp(after, Snapshot[0]) != 0)) {
            return fail("the passwd file was changed before its turn");
        }

        /* And the re-run refuses, naming the files that hold the leftovers,
         * except at the last point where the account is simply complete. */
        if (which == 4) {
            char script[128];

            if (run_adduser(CRASH_USER, "", report, sizeof(report)) == 0) {
                return fail("adduser created an account that was already complete");
            }
            if (strstr(report, "already exists") == NULL) {
                return fail("a complete account was reported as a residue");
            }
            /* The last prefix is finishable rather than wedged. */
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
 * Contention, on the windows this system actually has.
 *
 * `passwd` takes its transaction after its prompts and not before, so a writer
 * held at a prompt holds no lock: holding one across a terminal read was the
 * defect that ordering fixed, and no live writer here keeps the lock across
 * anything a test can synchronize on. So the deterministic half is built on the
 * lock a real writer really leaves. The crash build dies inside its
 * transaction, and against that lock `adduser` must say it is waiting, must
 * still be alive, and must have published nothing: an `adduser` that ignored
 * the lock would have read the pre-image and published all four replacements
 * inside this window. The lock is then removed the way root would, and the run
 * must complete.
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
    /* Removed the way root clears a crashed writer's residue, which is the only
     * thing that ever removes it: the library never breaks a lock. */
    (void)unlink(CRED_LOCK_PATH);
    (void)write(a_in[1], NEW_PASS "\n" NEW_PASS "\n", 2 * strlen(NEW_PASS) + 2);
    close(a_in[1]);
    if (waitpid(maker, &status, 0) != maker || !WIFEXITED(status)) {
        return fail("the waiting maker did not exit normally");
    }
    close(a_err[0]);
    /* It refuses: the crash left `crashed` in the shadow file, and this run is
     * for a different name, so what it must survive is the wait alone. */
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
 * Two real writers at once, and then two account makers at once.
 *
 * Nothing here is staged: both pairs are started together and race. What is
 * asserted is what a lost update destroys, that both results survive, and what
 * a number chosen outside the lock destroys, that two accounts made at once
 * have two different uids.
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
    /* Both survive, which is what a lost update destroys: either writer built
     * its replacement from an image that already held the other's line. */
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

    /* And two account makers at once, which must not agree on a number. Each
     * gets its own input pipe: one pipe shared between two exec'd `passwd`
     * children would have them racing for lines a byte at a time, and a probe
     * that failed that way would be failing at its own plumbing. */
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
            /* The status matters: each publishes its passwd record before it
             * execs `passwd`, so a run whose password never got set still
             * leaves the name in every file, and the uid assertions below would
             * pass over two half-made accounts. */
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
    /* And both were finished, not merely created: a `!` field is an account
     * whose creation succeeded and whose password never got set. */
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
 * The closing cross-check, in both directions, over whatever the run left.
 *
 * An `adduser` that ignored the lock leaves exactly the first failure, an `x`
 * field with no shadow line; one that checked only two databases leaves exactly
 * the second, a name appearing twice.
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
 * Run the cases, restore what they changed, and report.
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
