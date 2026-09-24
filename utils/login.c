/**
 * login: authenticate whoever is at the console and become them.
 *
 * init's session child execs this, so the console is behind a password rather
 * than in front of one. It has no set-id bit and needs none: init is root,
 * so this program starts as root and spends that authority once, dropping to
 * the account it just authenticated and never climbing back.
 *
 * It prompts for a name, reads the password through the credential library,
 * and verifies it against `/etc/shadow`. A failure sleeps and asks again, a
 * bounded number of times, and says the same thing whichever half was wrong: a
 * message that distinguished an unknown name from a wrong password would turn
 * the prompt into an oracle for which accounts exist. Running out of tries is
 * an exit, and init brings the prompt back.
 *
 * The identity drop is `setgroups`, `setgid`, `setuid`, in that order, and the
 * order matters: `setgroups` and `setgid` need the privilege that `setuid`
 * gives away, so either one after it is a call that fails while the process
 * keeps a group it was never entitled to. Each is checked, and a failure is
 * fatal, not a warning, since carrying on would mean running a shell as an
 * identity nobody chose.
 *
 * It does not `setsid` and does not claim the terminal. init's child already
 * did both, and the session and its controlling terminal survive the exec.
 *
 * The environment is built from nothing, not filtered, for the reason su
 * builds one: what arrives here came from init, which got it from the kernel,
 * and a login that passed a variable through would be handing the account
 * whatever the boot happened to carry. `PATH` is part of what it builds, and
 * has to be: a shell handed no `PATH` at all finds only programs named by a
 * full path, so the first bare `ls` after a login would fail. It is the same
 * `/bin:/usr/bin` su sets, so an account reaches the same programs however it
 * arrived.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <termios.h>
#include <sys/types.h>

#include "pwauth.h"

#define MOTD_FILE "/etc/motd"

/* How many names may be tried before the program gives up and init starts it
 * again. The tries are bounded so a wedged reader cannot hold the console; the
 * respawn is the retry. */
#define LOGIN_TRIES 3

/* The pause after a refusal, in seconds. It costs a legitimate user two
 * seconds and costs anyone working through a list of passwords the same two
 * seconds every time. */
#define LOGIN_PAUSE 2

/* The buffer for the longest account name that will be read, from the
 * library, so that what this reads and what `adduser` writes are one limit. An
 * over-long line is refused, not truncated, for the reason the password reader
 * refuses one: a prefix is a different name, and answering for it would
 * either fail confusingly or, worse, match somebody else. */
#define LOGIN_NAME_MAX (PWAUTH_MAX_NAME + 1)

/* Rach2 permits descriptors 0 through 1023. */
#define LOGIN_MAX_FDS 1024

/* SHA-512 comparison input for an absent passwd entry. */
#define LOGIN_DUMMY_HASH \
    "$6$rach2login$"     \
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

/**
 * Put the console back into the shape a login can be typed at. Returns
 * nothing.
 *
 * The terminal's settings are the terminal's, not the session's: they outlive
 * the account that changed them, so `stty -icanon min 0` followed by `exit` is
 * enough to leave every later login reading zero bytes at once, exiting, and
 * being respawned into the same failure forever. A getty resets the line before
 * it asks who is there, and this is that reset. Pending input goes with it,
 * since whatever was typed at the last session's shell is not this login's
 * answer.
 */
static void sane_terminal(void)
{
    struct termios modes;

    if (tcgetattr(STDIN_FILENO, &modes) != 0) {
        return;
    }
    modes.c_iflag |= ICRNL;
    modes.c_iflag &= ~(tcflag_t)(IGNCR | INLCR);
    modes.c_oflag |= OPOST | ONLCR;
    modes.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
    modes.c_cc[VMIN] = 1;
    modes.c_cc[VTIME] = 0;
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &modes);
}

/**
 * Read a name into `buf`, which must have at least one byte. Terminate every
 * successful result. Return 0 for a line or partial line at EOF, and -1 for
 * empty EOF or a read error. Discard an overlong line and return an empty
 * string. Read one byte at a time so the next prompt keeps its input.
 */
static int read_line(char *buf, size_t size)
{
    size_t len = 0;
    int over = 0;

    for (;;) {
        char c;
        ssize_t got = read(STDIN_FILENO, &c, 1);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }
        if (got == 0) {
            if (len == 0 && !over) {
                return -1;
            }
            break;
        }
        if (c == '\n' || c == '\r') {
            break;
        }
        if (len + 1 < size) {
            buf[len++] = c;
        } else {
            over = 1;
        }
    }
    buf[len] = '\0';
    if (over) {
        buf[0] = '\0';
    }

    return 0;
}

/**
 * Close every open descriptor above standard error before changing identity.
 *
 * The three standard ones are the session's terminal and stay. Anything else
 * open here was opened by whatever ran before this program, and an account that
 * inherited it would hold a descriptor on a file its own permissions may not
 * open at all.
 */
static void close_inherited(void)
{
    int fd;

    for (fd = 3; fd < LOGIN_MAX_FDS; fd++) {
        (void)close(fd);
    }
}

/**
 * Print `/etc/motd` to standard output, if it is there and readable.
 *
 * After the drop, deliberately: the file is the account's to read or not, and a
 * login that printed it as root would bypass the file's modes. A file that is
 * not there is not an error.
 */
static void print_motd(void)
{
    char buffer[512];
    int fd = open(MOTD_FILE, O_RDONLY);

    if (fd < 0) {
        return;
    }
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));

        if (got <= 0) {
            break;
        }
        if (write(STDOUT_FILENO, buffer, (size_t)got) != got) {
            break;
        }
    }
    close(fd);
}

/**
 * Become `pw` and exec its shell. Does not return: a failure to change
 * identity or to exec is reported and exits with status 1.
 *
 * The environment is the one this builds, the working directory is the
 * account's home, and `argv[0]` has the leading dash that tells a shell it
 * is a login shell. A home that cannot be entered is reported and the root
 * directory is used instead, since a missing home is a reason to complain and
 * not a reason to refuse a login the password already earned.
 */
static void become(const struct passwd *pw)
{
    char *argv[2];
    char *env[6];
    char home[256];
    char shell[256];
    char user[LOGIN_NAME_MAX + 8];
    char logname[LOGIN_NAME_MAX + 8];
    char arg0[256];
    const char *base;

    if (setgroups(0, NULL) != 0 || setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) {
        fprintf(stderr, "login: cannot become %s: %s\n", pw->pw_name, strerror(errno));
        _exit(1);
    }
    if (chdir(pw->pw_dir) != 0) {
        fprintf(stderr, "login: %s: %s; using /\n", pw->pw_dir, strerror(errno));
        (void)chdir("/");
    }
    print_motd();

    snprintf(home, sizeof(home), "HOME=%s", pw->pw_dir);
    snprintf(shell, sizeof(shell), "SHELL=%s", pw->pw_shell);
    snprintf(user, sizeof(user), "USER=%s", pw->pw_name);
    snprintf(logname, sizeof(logname), "LOGNAME=%s", pw->pw_name);
    env[0] = (char *)"PATH=/bin:/usr/bin";
    env[1] = home;
    env[2] = shell;
    env[3] = user;
    env[4] = logname;
    env[5] = NULL;

    base = strrchr(pw->pw_shell, '/');
    base = base ? base + 1 : pw->pw_shell;
    snprintf(arg0, sizeof(arg0), "-%s", base);
    argv[0] = arg0;
    argv[1] = NULL;
    execve(pw->pw_shell, argv, env);
    fprintf(stderr, "login: cannot exec %s: %s\n", pw->pw_shell, strerror(errno));
    _exit(1);
}

/**
 * Ask who is there, up to `LOGIN_TRIES` times, and become the first account
 * whose password is proved. Returns 1 when the name or the password cannot be
 * read, at end of input included, or when the tries run out.
 *
 * An empty name, which a bare newline or an over-long line both give, costs
 * no try and is never authenticated. A name with no passwd entry still has a
 * SHA-512 comparison made, against a dummy hash, so it takes as long as a
 * wrong password.
 */
int main(void)
{
    char name[LOGIN_NAME_MAX];
    char password[PWAUTH_MAX_PASSWORD + 1];
    int attempt;

    close_inherited();
    sane_terminal();
    for (attempt = 0; attempt < LOGIN_TRIES; attempt++) {
        struct passwd *pw;
        int proved;

        printf("rach2 login: ");
        fflush(stdout);
        if (read_line(name, sizeof(name)) != 0) {
            return 1;
        }
        if (name[0] == '\0') {
            attempt--;
            continue;
        }
        if (pwauth_read_password("Password: ", password, sizeof(password)) != 0) {
            return 1;
        }
        pw = getpwnam(name);
        proved = pwauth_verify(name, password);
        if (!pw) {
            (void)pwauth_check(LOGIN_DUMMY_HASH, password);
        }
        memset(password, 0, sizeof(password));
        if (proved && pw) {
            become(pw);

            return 1;
        }
        printf("login: incorrect\n");
        fflush(stdout);
        sleep(LOGIN_PAUSE);
    }
    printf("login: too many tries\n");
    fflush(stdout);

    return 1;
}
