/**
 * `login` in the guest: the program the console is now behind.
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
 * it uses the account the credential probes already committed: `probe` at uid
 * 1000, whose shell is the `/idreport` helper and whose home is `/home/probe`.
 * `idreport` prints the six ids and its `argv[0]`, which is exactly the triple
 * this program is answerable for plus the leading dash a login shell is told it
 * has. Asserting on that line is asserting on the identity a shell would start
 * with, not on an exit status that a refusal and a success can share.
 *
 * The refusals: a wrong password, an unknown name, and an account whose shadow
 * field is empty, which busybox's login would have admitted without asking
 * anything. Each must leave the databases as they were, must not exec, and must
 * be an ordinary exit, not a crash that happened to print nothing.
 *
 * Then the two ways to take the console for good, not for a moment: a
 * login stopped by an uncatchable `SIGSTOP`, which only its parent can undo,
 * and a terminal left the way `stty -icanon min 0` leaves it, which outlives
 * the session that did it and would have every later login read nothing, exit,
 * and be respawned into the same failure.
 *
 * Prints "LOGINMUSL OK" on success and "LOGINMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pty.h>
#include <termios.h>

#include "pwauth.h"

#define LOGIN_PATH "/login"
#define ETC_PASSWD "/etc/passwd"
#define ETC_SHADOW "/etc/shadow"
#define ETC_GROUP  "/etc/group"

#define PROBE_USER "probe"
#define PROBE_PASS "probepass"
#define PROBE_UID  1000

static const char *Databases[3] = {ETC_PASSWD, ETC_SHADOW, ETC_GROUP};
static char Snapshot[3][4096];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("LOGINMUSL FAIL (%s)\n", what);

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

/** Put the databases back the way every case here expects to find them. */
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
 * Read from `fd` until `needle` appears or `ms` elapse, appending to `buffer`.
 * Returns 1 on a match.
 */
static int read_until(int fd, const char *needle, char *buffer, size_t size, long ms)
{
    size_t held = strlen(buffer);
    long waited = 0;

    while (waited < ms) {
        struct pollfd watching;
        ssize_t got;

        watching.fd = fd;
        watching.events = POLLIN;
        watching.revents = 0;
        if (poll(&watching, 1, 200) == 1) {
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
 * The login that succeeds, asserted on the identity it hands over.
 */
static int check_login(void)
{
    char said[2048];
    char wanted[128];
    int status;

    restore_databases();
    status = run_login(PROBE_USER "\n" PROBE_PASS "\n", said, sizeof(said));
    if (status < 0) {
        return fail("could not run login");
    }
    if (strstr(said, "rach2 login:") == NULL) {
        return fail("login did not ask who was there");
    }
    /* The whole triple, and the dash. A real uid alone would pass on a program
     * that dropped the real id and kept the saved one, which is a process that
     * can climb back. */
    snprintf(wanted, sizeof(wanted), "uid=%d euid=%d suid=%d gid=%d egid=%d sgid=%d arg0=-idreport",
             PROBE_UID, PROBE_UID, PROBE_UID, PROBE_UID, PROBE_UID, PROBE_UID);
    if (strstr(said, wanted) == NULL) {
        return fail("login did not hand over the account's identity");
    }

    return 0;
}

/**
 * The refusals, none of which may exec anything.
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
            return fail("login admitted a caller who proved nothing");
        }
        /* And exited to say so. `run_login` reports -1 for a child that was
         * killed or faulted, and a crash that happened to print nothing about
         * a uid would otherwise be scored as a refusal. */
        if (status < 0) {
            return fail("login did not exit normally when it refused");
        }
        if (strstr(said, "incorrect") == NULL) {
            return fail("login did not report a refusal");
        }
        if (strstr(said, "uid=") != NULL) {
            return fail("a refused login still exec'd the account's shell");
        }
        /* The same words for an unknown name as for a wrong password: a
         * message that told them apart would be an oracle for which accounts
         * exist, which is a question a login prompt must not answer. */
        if (which == 1 && strstr(said, "nosuchaccount") != NULL) {
            return fail("login repeated an unknown name back at the prompt");
        }
    }

    /* An empty shadow field is not a password everyone knows: it is no
     * password at all, and busybox's login would have admitted it. */
    restore_databases();
    if (slurp(ETC_SHADOW, staged, sizeof(staged)) < 0 || set_hash(staged, PROBE_USER, "") != 0 ||
        spill(ETC_SHADOW, staged, 0600) != 0) {
        return fail("could not stage an account with an empty password field");
    }
    status = run_login(PROBE_USER "\n\n"
                                  "\n\n\n\n",
                       said, sizeof(said));
    if (status <= 0 || strstr(said, "uid=") != NULL) {
        return fail("login admitted an account whose password field is empty");
    }
    restore_databases();

    return 0;
}

/**
 * A login that was stopped, and a terminal somebody left unusable.
 *
 * Both are ways to take the console away for good, not for a moment, and
 * neither can be fixed by the process they happen to: a stop cannot be caught
 * when it arrives as `SIGSTOP`, and the terminal's modes outlive every session
 * that changes them. So one is init's to undo and the other is the login's to
 * undo before it asks anything.
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

    /* A stopped login: the probe plays init, and must be able to bring it
     * back. A `wait` without WUNTRACED never returns here at all. */
    if (pipe(in) != 0 || pipe(out) != 0) {
        return fail("could not make the pipes the stop case needs");
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
        return fail("could not fork a login to stop");
    }
    if (kill(child, SIGSTOP) != 0) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);

        return fail("could not stop the login");
    }
    if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status)) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);

        return fail("a stopped login was not reported as stopped");
    }
    if (kill(child, SIGCONT) != 0) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);

        return fail("could not continue the stopped login");
    }
    (void)write(in[1], PROBE_USER "\n" PROBE_PASS "\n", 0);
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
        return fail("the continued login did not run to an ordinary end");
    }
    if (strstr(said, "uid=") == NULL) {
        return fail("a login that was stopped and continued never came back");
    }

    /* A terminal left the way `stty -icanon min 0` leaves it. A login that did
     * not put the line back would read zero bytes at once, exit, and be
     * respawned into the same failure forever. It takes a real terminal to see
     * this at all: through a pipe there is nothing to reset, and a login whose
     * standard input is a pipe never asks. */
    {
        int master = -1;
        int slave = -1;
        struct termios asked;
        int sane;

        if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
            return fail("could not open a pty to wreck");
        }
        if (tcgetattr(slave, &wrecked) != 0) {
            close(master);
            close(slave);

            return fail("the pty has no modes to read");
        }
        wrecked.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        wrecked.c_cc[VMIN] = 0;
        wrecked.c_cc[VTIME] = 0;
        if (tcsetattr(slave, TCSANOW, &wrecked) != 0) {
            close(master);
            close(slave);

            return fail("could not leave the pty the way a session can");
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

            return fail("could not fork a login onto the wrecked pty");
        }
        sane = 0;
        {
            char seen[2048];

            seen[0] = '\0';
            /* The prompt is the rendezvous, and it has to be: the reset flushes
             * whatever was already typed, so a name written before login got
             * that far would be discarded by the very repair being tested. */
            if (read_until(master, "rach2 login:", seen, sizeof(seen), 20000)) {
                (void)write(master, PROBE_USER "\n", strlen(PROBE_USER) + 1);
                if (read_until(master, "Password:", seen, sizeof(seen), 20000)) {
                    (void)write(master, PROBE_PASS "\n", strlen(PROBE_PASS) + 1);
                    (void)read_until(master, "uid=", seen, sizeof(seen), 20000);
                }
            }
            /* Read back through the slave the login is using, since that is
             * the line whose modes it was answerable for. */
            if (tcgetattr(slave, &asked) == 0 && (asked.c_lflag & ICANON) != 0 &&
                asked.c_cc[VMIN] == 1) {
                sane = 1;
            }
            if (strstr(seen, "uid=") == NULL) {
                kill(child, SIGKILL);
                (void)waitpid(child, &status, 0);
                close(master);
                close(slave);

                return fail("login could not be used on a terminal a session had wrecked");
            }
        }
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        close(master);
        close(slave);
        if (!sane) {
            return fail("login did not put the terminal back into canonical mode");
        }
    }

    return 0;
}

/**
 * Run the cases and report.
 */
int main(void)
{
    size_t i;
    int rc;

    if (getuid() != 0) {
        printf("LOGINMUSL FAIL (the probe must run as root)\n");

        return 1;
    }
    for (i = 0; i < 3; i++) {
        if (slurp(Databases[i], Snapshot[i], sizeof(Snapshot[i])) < 0) {
            printf("LOGINMUSL FAIL (cannot read the databases)\n");

            return 1;
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
        printf("LOGINMUSL OK\n");
    }

    return rc;
}
