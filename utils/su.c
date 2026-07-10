/**
 * su: become another user (root by default) after proving their password.
 *
 * This is Rach2's su, the companion to the reference sudo and the payoff of the
 * setuid milestone: a small setuid-root program that authenticates against
 * `/etc/shadow` and then runs a shell as the target. BusyBox ships its own su,
 * but it needs the whole busybox binary to be setuid root, and a setuid busybox
 * must then be confined so only su keeps the privilege. Rach2 keeps the setuid
 * surface a hundred lines instead of a megabyte: busybox stays unprivileged and
 * this is the one program that climbs.
 *
 * Usage: `su [user] [-c command]`, user defaulting to root. The caller proves
 * the target's password (root is exempt, as always), read from standard input;
 * su resolves the target through `getpwnam`, verifies the `/etc/shadow` hash
 * with `crypt`, drops to the target's group and then user (the order matters:
 * privilege to set the gid is gone once the uid drops), builds the target's
 * environment from nothing rather than inherit the caller's (the shell expands
 * variables like `PATH` and `PS1`, so a carried-over one could run the caller's
 * code as the target), and execs the target's shell.
 *
 * Deliberately left for the exercise: reading the password from the controlling
 * terminal with echo disabled (it is read from stdin here, so it echoes and can
 * come from a pipe), the login (`-`) semantics beyond the environment, and the
 * PAM/login.defs policy layer.
 */

#define _GNU_SOURCE /* crypt, getspnam, setgroups */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>

/* Above the kernel's per-process descriptor cap (MAX_FDS is 16). */
#define MAX_FD 64

/* Read one line from stdin into `buf`, stripping the newline. Returns 0 on
 * success, -1 on end of input. */
static int read_line(char *buf, size_t size)
{
    if (fgets(buf, (int) size, stdin) == NULL) {
        return -1;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

/* Verify `password` against the target's /etc/shadow hash. Returns 1 if it
 * matches, 0 otherwise. A missing shadow entry or a locked (non-`$`) hash
 * denies everyone. */
static int password_ok(const char *user, const char *password)
{
    struct spwd *sp = getspnam(user);
    char *hash;

    if (!sp || !sp->sp_pwdp || sp->sp_pwdp[0] != '$') {
        return 0;
    }
    hash = crypt(password, sp->sp_pwdp);
    return hash != NULL && strcmp(hash, sp->sp_pwdp) == 0;
}

int main(int argc, char **argv)
{
    const char *user = "root";
    const char *command = NULL;
    struct passwd *pw;
    int i;

    int fd;

    /* A bare "-" and other options are accepted and ignored; implementing the
     * login semantics is part of the exercise. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (argv[i][0] != '-') {
            user = argv[i];
        }
    }

    pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "su: unknown user %s\n", user);
        return 1;
    }

    /* Root becomes anyone without proof; everyone else proves the target's
     * password, read from stdin. */
    if (getuid() != 0) {
        char password[256];

        if (read_line(password, sizeof(password)) != 0 ||
            !password_ok(user, password)) {
            fprintf(stderr, "su: authentication failure\n");
            return 1;
        }
    }

    /* Nothing the caller left open should reach the target's shell. */
    for (fd = 3; fd < MAX_FD; fd++) {
        close(fd);
    }

    /* Shed the caller's supplementary groups, then take the target's group and
     * user. The order is forced: each step needs a privilege the uid drop
     * removes, and the image's accounts carry no supplementary groups, so
     * clearing the list is the whole of it. */
    if (setgroups(0, NULL) != 0 ||
        setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) {
        fprintf(stderr, "su: cannot assume the target identity\n");
        return 1;
    }

    /* Only these five reach the target's shell (see the header on why the
     * caller's environment is discarded, not patched). */
    {
        static char home[128], shell[128], usr[96], logname[96];
        char *const env[] = {
            (char *) "PATH=/bin:/usr/bin", home, shell, usr, logname, NULL
        };

        snprintf(home, sizeof(home), "HOME=%s", pw->pw_dir);
        snprintf(shell, sizeof(shell), "SHELL=%s", pw->pw_shell);
        snprintf(usr, sizeof(usr), "USER=%s", pw->pw_name);
        snprintf(logname, sizeof(logname), "LOGNAME=%s", pw->pw_name);

        if (command) {
            execle(pw->pw_shell, pw->pw_shell, "-c", command, (char *) 0, env);
        } else {
            execle(pw->pw_shell, pw->pw_shell, (char *) 0, env);
        }
    }
    fprintf(stderr, "su: cannot exec %s\n", pw->pw_shell);
    return 1;
}
