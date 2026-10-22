/**
 * loginmusl: check `login`, the program the console is behind, in the guest.
 *
 * link-deps: utils/pwauth.c
 *
 * The harness cannot boot a console login at all: it types `exec <name>` at the
 * kernel debug shell against an image with no `/sbin/init`, so neither init nor
 * login runs there. What can be driven is the program itself, with a pipe for
 * fd 0, and what it does is the whole of what matters: it either becomes an
 * account or it does not.
 *
 * The success case needs something to exec, and this image stages no shell, so
 * it uses the account the credential probes already stage: `probe` at uid
 * 1000, whose shell is the `/idreport` helper and whose home is `/home/probe`.
 * `idreport` prints the six ids and its `argv[0]`, which are the ids this
 * program sets plus the leading dash that tells a shell it is a login shell.
 * The check is on that line, the identity a shell would start with, since an
 * exit status can be the same for a refusal and a success.
 *
 * The refusals: a wrong password, an unknown name, and an account whose shadow
 * field is empty, which BusyBox's login admits without asking anything. Each
 * must not exec, and must be an ordinary exit, not a crash that happened to
 * print nothing. The databases are written back before each case.
 *
 * Then the two ways to take the console for good, not for a moment: a login
 * stopped by an uncatchable `SIGSTOP`, which only its parent can undo, and a
 * terminal left the way `stty -icanon -echo min 0` leaves it, which outlives
 * the session that did it and would have every later login read nothing,
 * exit, and be respawned into the same failure.
 *
 * Prints "LOGINMUSL OK" on success and "LOGINMUSL FAIL (detail)" otherwise.
 * Writes etude's report records (../probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check `checks` lists, those of the two refused
 * scripts once each; a failure leaves the checks after it not run.
 *
 * A native run gives `ids=OLD:NEW,...` (tests/native/Readme.md) and a private
 * root whose account files and home have `probe` at the NEW of 1000; the
 * identity the probe then requires of `idreport` is that uid.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pty.h>
#include <termios.h>

#include "pwauth.h"
#include "../probe_args.h"
#include "../probe_report.h"

#define LOGIN_PATH "/login"
#define ETC_PASSWD "/etc/passwd"
#define ETC_SHADOW "/etc/shadow"
#define ETC_GROUP  "/etc/group"

#define PROBE_USER "probe"
#define PROBE_PASS "probepass"
#define PROBE_UID  ((int)probe_id(1000))

/* The three databases, and their contents as `main` found them, which
 * `restore_databases` writes back. */
static const char *Databases[3] = {ETC_PASSWD, ETC_SHADOW, ETC_GROUP};
static char Snapshot[3][4096];

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    MUST_BE_ROOT,
    DATABASES,
    RUN_LOGIN,
    PROMPT,
    IDENTITY,
    ADMITTED,
    NORMAL_EXIT,
    REFUSAL_REPORTED,
    NO_SHELL,
    NAME_NOT_REPEATED,
    STAGE_EMPTY,
    EMPTY_REFUSED,
    STOP_PIPES,
    STOP_FORK,
    STOP,
    STOPPED_REPORTED,
    CONTINUE,
    CONTINUED_ENDS,
    CONTINUED_LOGS_IN,
    PTY_OPEN,
    PTY_MODES,
    PTY_WRECK,
    PTY_FORK,
    WRECKED_USABLE,
    CANONICAL,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"root", "the probe must run as root"},
    {"databases", "cannot read the databases"},
    {"run-login", "could not run login"},
    {"prompt", "login did not ask who was there"},
    {"identity", "login did not hand over the account's identity"},
    {"admitted", "login admitted a caller who proved nothing"},
    {"normal-exit", "login did not exit normally when it refused"},
    {"refusal-reported", "login did not report a refusal"},
    {"no-shell", "a refused login still exec'd the account's shell"},
    {"name-not-repeated", "login repeated an unknown name back at the prompt"},
    {"stage-empty", "could not stage an account with an empty password field"},
    {"empty-refused", "login admitted an account whose password field is empty"},
    {"stop-pipes", "could not make the pipes the stop case needs"},
    {"stop-fork", "could not fork a login to stop"},
    {"stop", "could not stop the login"},
    {"stopped-reported", "a stopped login was not reported as stopped"},
    {"continue", "could not continue the stopped login"},
    {"continued-ends", "the continued login did not run to an ordinary end"},
    {"continued-logs-in", "a login that was stopped and continued never came back"},
    {"pty-open", "could not open a pty to wreck"},
    {"pty-modes", "the pty has no modes to read"},
    {"pty-wreck", "could not leave the pty the way a session can"},
    {"pty-fork", "could not fork a login onto the wrecked pty"},
    {"wrecked-usable", "login could not be used on a terminal a session had wrecked"},
    {"canonical", "login did not put the terminal back into canonical mode"},
};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with its text, those after it as not run, and
 * the report's end; with `check` CHECKS, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, checks[i].text);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Report the failed check `check` and return 1, the probe's failure
 * status. */
static int fail(int check)
{
    printf("LOGINMUSL FAIL (%s)\n", checks[check].text);
    report_at(check);

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

/**
 * Write the snapshots back to the databases, the shadow file at 0600 and the
 * others at 0644. A failed write is ignored.
 */
static void restore_databases(void)
{
    size_t i;

    for (i = 0; i < 3; i++) {
        (void)spill(Databases[i], Snapshot[i], i == 1 ? 0600 : 0644);
    }
}

/**
 * Replace the hash field of `user`'s shadow line with `value`, in place.
 * Returns 0 when it was done.
 */
static int set_hash(char *text, const char *user, const char *value)
{
    size_t len = strlen(user);
    char *line = text;

    while (*line) {
        char *end = strchr(line, '\n');
        size_t span = end ? (size_t)(end - line) : strlen(line);

        if (span > len && strncmp(line, user, len) == 0 && line[len] == ':') {
            char *field = line + len + 1;
            char *stop = strchr(field, ':');

            if (!stop) {
                return -1;
            }
            memmove(field + strlen(value), stop, strlen(stop) + 1);
            memcpy(field, value, strlen(value));

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
 * Read from `fd`, appending to the terminated string in `buffer` of `size`
 * bytes, until `needle` appears in it. Returns 1 on a match, and 0 at end of
 * file, on a read error, or when the buffer is full.
 */
static int read_until(int fd, const char *needle, char *buffer, size_t size)
{
    size_t held = strlen(buffer);
    ssize_t got;

    for (;;) {
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
    }
}

/**
 * Run `/login` with `script` on its standard input, collecting what it and
 * whatever it execs write to standard output and standard error into `said`.
 * Returns the child's exit status, or -1.
 *
 * Both streams go to the one pipe because the interesting output is split
 * across them by design: the prompts and the refusal are the program's, and the
 * id line belongs to the program it becomes.
 */
static int run_login(const char *script, char *said, size_t size)
{
    int in[2];
    int out[2];
    size_t held = 0;
    int status;
    pid_t child;

    said[0] = '\0';
    if (pipe(in) != 0) {
        return -1;
    }
    if (pipe(out) != 0) {
        close(in[0]);
        close(in[1]);

        return -1;
    }
    child = fork();
    if (child == 0) {
        close(in[1]);
        close(out[0]);
        if (dup2(in[0], 0) < 0 || dup2(out[1], 1) < 0 || dup2(out[1], 2) < 0) {
            _exit(90);
        }
        execl(LOGIN_PATH, "login", (char *)0);
        _exit(91);
    }
    close(in[0]);
    close(out[1]);
    if (child < 0) {
        close(in[1]);
        close(out[0]);

        return -1;
    }
    if (script && *script) {
        (void)write(in[1], script, strlen(script));
    }
    close(in[1]);
    for (;;) {
        ssize_t got = read(out[0], said + held, size - held - 1);

        if (got <= 0) {
            break;
        }
        held += (size_t)got;
        said[held] = '\0';
        if (held + 1 >= size) {
            break;
        }
    }
    close(out[0]);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

/**
 * Check the login that succeeds: it prompts, and the program it execs has
 * every uid and gid of `probe` and the leading dash. Every one of the six ids
 * is checked, since a real uid alone would pass for a program that set the
 * real id and kept the saved one, a process that can regain root. Returns 0,
 * or 1 after printing the failure.
 */
static int check_login(void)
{
    char said[2048];
    char wanted[128];
    int status;

    restore_databases();
    status = run_login(PROBE_USER "\n" PROBE_PASS "\n", said, sizeof(said));
    if (status < 0) {
        return fail(RUN_LOGIN);
    }
    if (strstr(said, "rach2 login:") == NULL) {
        return fail(PROMPT);
    }
    snprintf(wanted, sizeof(wanted), "uid=%d euid=%d suid=%d gid=%d egid=%d sgid=%d arg0=-idreport",
             PROBE_UID, PROBE_UID, PROBE_UID, PROBE_UID, PROBE_UID, PROBE_UID);
    if (strstr(said, wanted) == NULL) {
        return fail(IDENTITY);
    }

    return 0;
}

/**
 * Check the refusals, none of which may exec anything: a wrong password, an
 * unknown name, and an empty shadow field, which is no password at all.
 *
 * Each must end in an ordinary exit: `run_login` returns -1 for a child that
 * was killed or faulted, and a crash that printed nothing about a uid would
 * otherwise count as a refusal. An unknown name gets the same words as a
 * wrong password and is not repeated back, since a message that told them
 * apart would show which accounts exist. Returns 0, or 1 after printing the
 * failure.
 */
static int check_refusals(void)
{
    static const char *const scripts[] = {PROBE_USER "\nwrong-password\n"
                                                     "\n\n\n\n",
                                          "nosuchaccount\n" PROBE_PASS "\n"
                                          "\n\n\n\n"};
    char said[2048];
    char staged[4096];
    size_t which;
    int status;

    for (which = 0; which < sizeof(scripts) / sizeof(scripts[0]); which++) {
        restore_databases();
        status = run_login(scripts[which], said, sizeof(said));
        if (status == 0) {
            return fail(ADMITTED);
        }
        if (status < 0) {
            return fail(NORMAL_EXIT);
        }
        if (strstr(said, "incorrect") == NULL) {
            return fail(REFUSAL_REPORTED);
        }
        if (strstr(said, "uid=") != NULL) {
            return fail(NO_SHELL);
        }
        if (which == 1 && strstr(said, "nosuchaccount") != NULL) {
            return fail(NAME_NOT_REPEATED);
        }
    }

    /* An empty shadow field. */
    restore_databases();
    if (slurp(ETC_SHADOW, staged, sizeof(staged)) < 0 || set_hash(staged, PROBE_USER, "") != 0 ||
        spill(ETC_SHADOW, staged, 0600) != 0) {
        return fail(STAGE_EMPTY);
    }
    status = run_login(PROBE_USER "\n\n"
                                  "\n\n\n\n",
                       said, sizeof(said));
    if (status <= 0 || strstr(said, "uid=") != NULL) {
        return fail(EMPTY_REFUSED);
    }
    restore_databases();

    return 0;
}

/**
 * Check a login that was stopped, and a terminal a session left unusable.
 * Returns 0, or 1 after printing the failure.
 *
 * Both are ways to take the console away for good, and neither can be fixed
 * by the process they happen to: a stop cannot be caught when it arrives as
 * `SIGSTOP`, and the terminal's modes outlive every session that changes
 * them. So one is init's to undo and the other is the login's to undo before
 * it prompts.
 *
 * For the stop, this process plays init: it must see the stop with `WUNTRACED`,
 * without which the wait would never return, and continue the login, which must
 * then log the account in. For the terminal, a pty is left as
 * `stty -icanon -echo min 0` leaves a line, and a login started on it must put
 * it back into canonical mode, read through the slave the login uses. A login
 * that did not would read zero bytes at once, exit, and be respawned into the
 * same failure forever; only a terminal shows this, since a login on a pipe
 * has no modes to reset. The name is written only once the prompt appears,
 * since the reset discards input already queued, and a name written earlier
 * would be lost to the repair being tested.
 */
static int check_wedges(void)
{
    char said[2048];
    struct termios wrecked;
    int in[2];
    int out[2];
    int status;
    pid_t child;

    restore_databases();

    /* A stopped login. */
    if (pipe(in) != 0 || pipe(out) != 0) {
        return fail(STOP_PIPES);
    }
    child = fork();
    if (child == 0) {
        close(in[1]);
        close(out[0]);
        if (dup2(in[0], 0) < 0 || dup2(out[1], 1) < 0 || dup2(out[1], 2) < 0) {
            _exit(90);
        }
        execl(LOGIN_PATH, "login", (char *)0);
        _exit(91);
    }
    close(in[0]);
    close(out[1]);
    if (child < 0) {
        return fail(STOP_FORK);
    }
    if (kill(child, SIGSTOP) != 0) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);

        return fail(STOP);
    }
    if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status)) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);

        return fail(STOPPED_REPORTED);
    }
    if (kill(child, SIGCONT) != 0) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);

        return fail(CONTINUE);
    }
    (void)write(in[1], PROBE_USER "\n", strlen(PROBE_USER) + 1);
    (void)write(in[1], PROBE_PASS "\n", strlen(PROBE_PASS) + 1);
    close(in[1]);
    said[0] = '\0';
    {
        size_t held = 0;

        for (;;) {
            ssize_t got = read(out[0], said + held, sizeof(said) - held - 1);

            if (got <= 0) {
                break;
            }
            held += (size_t)got;
            said[held] = '\0';
            if (held + 1 >= sizeof(said)) {
                break;
            }
        }
    }
    close(out[0]);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return fail(CONTINUED_ENDS);
    }
    if (strstr(said, "uid=") == NULL) {
        return fail(CONTINUED_LOGS_IN);
    }

    /* A terminal left as `stty -icanon -echo min 0` leaves it. */
    {
        int master = -1;
        int slave = -1;
        struct termios asked;
        int sane;

        if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
            return fail(PTY_OPEN);
        }
        if (tcgetattr(slave, &wrecked) != 0) {
            close(master);
            close(slave);

            return fail(PTY_MODES);
        }
        wrecked.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        wrecked.c_cc[VMIN] = 0;
        wrecked.c_cc[VTIME] = 0;
        if (tcsetattr(slave, TCSANOW, &wrecked) != 0) {
            close(master);
            close(slave);

            return fail(PTY_WRECK);
        }
        child = fork();
        if (child == 0) {
            close(master);
            if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0) {
                _exit(90);
            }
            execl(LOGIN_PATH, "login", (char *)0);
            _exit(91);
        }
        if (child < 0) {
            close(master);
            close(slave);

            return fail(PTY_FORK);
        }
        sane = 0;
        {
            char seen[2048];

            seen[0] = '\0';
            if (read_until(master, "rach2 login:", seen, sizeof(seen))) {
                (void)write(master, PROBE_USER "\n", strlen(PROBE_USER) + 1);
                if (read_until(master, "Password:", seen, sizeof(seen))) {
                    (void)write(master, PROBE_PASS "\n", strlen(PROBE_PASS) + 1);
                    (void)read_until(master, "uid=", seen, sizeof(seen));
                }
            }
            if (tcgetattr(slave, &asked) == 0 && (asked.c_lflag & ICANON) != 0 &&
                asked.c_cc[VMIN] == 1) {
                sane = 1;
            }
            if (strstr(seen, "uid=") == NULL) {
                kill(child, SIGKILL);
                (void)waitpid(child, &status, 0);
                close(master);
                close(slave);

                return fail(WRECKED_USABLE);
            }
        }
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        close(master);
        close(slave);
        if (!sane) {
            return fail(CANONICAL);
        }
    }

    return 0;
}

/**
 * Take the snapshots, run the cases, write the databases back and print the
 * result. Must run as root. Returns 0 when every case passes, 1 otherwise.
 */
int main(int argc, char **argv)
{
    size_t i;
    int rc;

    probe_args(argc, argv);
    report_begin(report_case(environ, "guest.default.loginmusl"));
    if (getuid() != 0) {
        return fail(MUST_BE_ROOT);
    }
    for (i = 0; i < 3; i++) {
        if (slurp(Databases[i], Snapshot[i], sizeof(Snapshot[i])) < 0) {
            return fail(DATABASES);
        }
    }
    rc = check_login();
    if (rc == 0) {
        rc = check_refusals();
    }
    if (rc == 0) {
        rc = check_wedges();
    }
    restore_databases();
    if (rc == 0) {
        report_at(CHECKS);
        printf("LOGINMUSL OK\n");
    }

    return rc;
}
