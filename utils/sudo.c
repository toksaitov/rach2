/**
 * sudo: run a command as root, if the real user is permitted.
 *
 * This is Rach2's reference sudo, the answer key to the coursework exercise:
 * the kernel gives a setuid program everything it needs and nothing more, and
 * a working sudo is about a hundred lines on top of it. It is staged only into
 * the test image, gated by `sudoprobe`; the shipped os-real image ships `su`
 * and leaves this for the student to write.
 *
 * The program runs setuid root (the exec gave it effective uid 0 while the real
 * uid still names the caller). It reads `/etc/sudoers`, a list of the numeric
 * uids allowed to use it, one per line, `#` starting a comment. The check is
 * against the REAL uid, the identity the caller cannot forge, not the effective
 * uid the setuid bit just handed over: that is the whole lesson. A permitted
 * caller gets a clean environment (an attacker must not steer a root program
 * through PATH or the like), full root via the saved id, and the requested
 * command; anyone else is refused.
 *
 * Usage: `sudo <command> [args...]`. Deliberately omitted, and left for the
 * exercise to add: per-command rules, a password prompt, and TTY handling.
 */

#define _GNU_SOURCE /* setresuid */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUDOERS "/etc/sudoers"

/* A fixed, minimal environment for the command: never the caller's, which a
 * setuid program cannot trust. */
static char *const clean_env[] = {(char *)"PATH=/bin:/usr/bin", (char *)"HOME=/root", NULL};

/* True if `uid` appears on its own line in /etc/sudoers (decimal, `#` comments
 * and blank lines ignored). A missing or unreadable file permits no one. */
static int permitted(unsigned long uid)
{
    FILE *f = fopen(SUDOERS, "r");
    char line[128];
    int ok = 0;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        char *end;
        unsigned long v;

        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '#' || *s == '\n' || *s == '\0') {
            continue;
        }
        v = strtoul(s, &end, 10);
        while (*end == ' ' || *end == '\t' || *end == '\n') {
            end++;
        }
        if (end != s && *end == '\0' && v == uid) {
            ok = 1;
            break;
        }
    }
    fclose(f);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned long ruid = (unsigned long)getuid();

    if (argc < 2) {
        fprintf(stderr, "usage: sudo <command> [args...]\n");
        return 2;
    }

    if (!permitted(ruid)) {
        fprintf(stderr, "sudo: uid %lu is not permitted\n", ruid);
        return 1;
    }

    /* Become root outright: the saved uid is 0 (the setuid exec set it), so
     * this also fixes the real uid, and the command cannot swing back. */
    if (setresuid(0, 0, 0) != 0 || setresgid(0, 0, 0) != 0) {
        fprintf(stderr, "sudo: could not assume root\n");
        return 1;
    }

    execve(argv[1], &argv[1], clean_env);
    fprintf(stderr, "sudo: %s: cannot execute\n", argv[1]);
    return 1;
}
