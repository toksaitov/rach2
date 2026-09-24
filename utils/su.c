/**
 * su: become another user (root by default) after proving their password.
 *
 * This is Rach2's su, the companion to the reference sudo: a small setuid-root
 * program that authenticates against `/etc/shadow` and then runs a shell as
 * the target. It is os-simple's way up. os-real climbs through busybox's own
 * su instead, from a second build of three applets, since making the whole
 * megabyte setuid would hand every applet in it root. The two images run the
 * same job in this project's code and in the code a real system ships.
 *
 * Usage: `su [-] [-l|--login] [user] [-c command]`, user defaulting to root.
 * The caller proves the target's password (root is exempt, as always), read
 * through the credential library so the terminal does not echo it; su resolves
 * the target through `getpwnam`, verifies the `/etc/shadow` hash, drops to the
 * target's group and then user (the order matters: privilege to set the gid is
 * gone once the uid drops), builds the target's environment from nothing rather
 * than inherit the caller's (the shell expands variables like `PATH` and `PS1`,
 * so a carried-over one could run the caller's code as the target), and execs
 * the target's shell.
 *
 * Login mode is three things and is entered by a bare `-`, by `-l`, or by
 * `--login`: the target's home becomes the working directory, `argv[0]` becomes
 * a `-` followed by the shell's base name (`-sh` for `/bin/sh`), which is how a
 * shell here is told it is a login shell, and the environment is the one su
 * already builds from nothing. A home that cannot be entered is fatal rather
 * than a warning: the directory and the dash are the whole of `-`, so
 * continuing would run a login shell in the caller's directory and call it a
 * login. The home is entered as the target, so one the target cannot reach
 * refuses the login instead of being opened with root's privilege. Without
 * login mode the working directory stays the caller's and `argv[0]` is the
 * shell's path.
 *
 * The target gets no supplementary groups: the list is cleared, and the
 * image's accounts have none to add.
 *
 * The password comparison, the terminal handling, and the signal handling
 * belong to `pwauth.c`, which is also what `passwd`, `adduser` and `login`
 * authenticate through. su holds no credential lock: a reader takes none, so no
 * stuck writer can keep anyone out of the box. There is no PAM or login.defs
 * policy layer.
 */

#define _GNU_SOURCE /* setgroups */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>

#include "pwauth.h"

/* Rach2 permits descriptors 0 through 1023. */
#define MAX_FD 1024

/** Print the usage line and return the exit status a usage error takes. */
static int usage(void)
{
    fprintf(stderr, "usage: su [-] [-l|--login] [user] [-c command]\n");
    return 2;
}

/**
 * Return the base name of `path`: `sh` for `/bin/sh`, the whole string when it
 * has no slash.
 *
 * A trailing slash is not special-cased, since a shell path that ends in one
 * cannot be executed anyway and would fail at the exec with its own message.
 */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

/**
 * Parse the arguments, authenticate the caller as the target, and exec the
 * target's shell as the target.
 *
 * Every argument is accounted for: an unrecognized option is a usage error,
 * never a discarded one, and a second operand is one too. The real root user
 * bypasses password verification. Other callers must authenticate the target
 * account. An unknown target is reported before prompting. Every descriptor
 * above standard error is closed before the identity changes.
 */
int main(int argc, char **argv)
{
    const char *user = NULL;
    const char *command = NULL;
    const char *arg0;
    struct passwd *pw;
    int login = 0;
    int i, fd;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0 ||
            strcmp(argv[i], "--login") == 0) {
            login = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                return usage();
            }
            command = argv[++i];
        } else if (argv[i][0] == '-' || user) {
            return usage();
        } else {
            user = argv[i];
        }
    }
    if (!user) {
        user = "root";
    }

    pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "su: unknown user %s\n", user);
        return 1;
    }

    if (getuid() != 0) {
        char password[256];

        if (pwauth_read_password("Password: ", password, sizeof(password)) != 0 ||
            !pwauth_verify(user, password)) {
            fprintf(stderr, "su: authentication failure\n");
            return 1;
        }
    }

    for (fd = 3; fd < MAX_FD; fd++) {
        close(fd);
    }

    if (setgroups(0, NULL) != 0 || setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) {
        fprintf(stderr, "su: cannot assume the target identity\n");
        return 1;
    }

    if (login && chdir(pw->pw_dir) != 0) {
        fprintf(stderr, "su: cannot enter %s\n", pw->pw_dir);
        return 1;
    }

    {
        static char home[128], shell[128], usr[96], logname[96], dashed[128];
        char *const env[] = {(char *)"PATH=/bin:/usr/bin", home, shell, usr, logname, NULL};

        snprintf(home, sizeof(home), "HOME=%s", pw->pw_dir);
        snprintf(shell, sizeof(shell), "SHELL=%s", pw->pw_shell);
        snprintf(usr, sizeof(usr), "USER=%s", pw->pw_name);
        snprintf(logname, sizeof(logname), "LOGNAME=%s", pw->pw_name);

        arg0 = pw->pw_shell;
        if (login) {
            snprintf(dashed, sizeof(dashed), "-%s", base_name(pw->pw_shell));
            arg0 = dashed;
        }

        if (command) {
            execle(pw->pw_shell, arg0, "-c", command, (char *)0, env);
        } else {
            execle(pw->pw_shell, arg0, (char *)0, env);
        }
    }
    fprintf(stderr, "su: cannot exec %s\n", pw->pw_shell);
    return 1;
}
