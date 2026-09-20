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
#include <errno.h>

#define SUDOERS "/etc/sudoers"

/* Rach2 permits descriptors 0 through 1023. */
#define MAX_FD 1024

/* A fixed, minimal environment for the command: never the caller's, which a
 * setuid program cannot trust. */
static char *const clean_env[] = {(char *)"PATH=/bin:/usr/bin", (char *)"HOME=/root", NULL};

/**
 * Read one line into `line`, returning its length, -1 at the end, or -2 for a
 * line this reader refuses: one with a NUL byte or longer than the buffer.
 */
static int sudoers_line(FILE *f, char *line, size_t size)
{
    size_t len = 0;
    int c, bad = 0;

    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (c == 0 || len == size - 1) {
            bad = 1;
        } else {
            line[len++] = (char)c;
        }
    }
    if (ferror(f) || (c == EOF && len == 0 && !bad)) {
        return -1;
    }
    if (bad) {
        return -2;
    }
    line[len] = '\0';
    return (int)len;
}

/**
 * Return whether `line` names `uid` and nothing else.
 *
 * A line is the uid in decimal with space around it and nothing more, so a
 * uid written inside a comment or beside another word permits nobody.
 */
static int line_names_uid(const char *line, unsigned long uid)
{
    const char *s = line;
    char *end;
    unsigned long v;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return 0;
    }
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno == ERANGE || end == s) {
        return 0;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    return *end == '\0' && v == uid;
}

/**
 * Return 1 when a complete line of /etc/sudoers names `uid` and nothing else.
 *
 * Blank lines and comments are skipped, and a line this reader refuses is
 * skipped rather than read in part. A missing or unreadable file permits no
 * one.
 */
static int permitted(unsigned long uid)
{
    FILE *f = fopen(SUDOERS, "r");
    char line[128];
    int ok = 0;

    if (!f) {
        return 0;
    }
    for (;;) {
        int len = sudoers_line(f, line, sizeof(line));

        if (len == -1) {
            break;
        }
        if (len == -2) {
            continue;
        }
        if (line_names_uid(line, uid)) {
            ok = 1;
            break;
        }
    }
    fclose(f);
    return ok;
}

/**
 * Close every open descriptor above standard error before changing identity.
 */
int main(int argc, char **argv)
{
    unsigned long ruid = (unsigned long)getuid();
    int fd;

    if (argc < 2) {
        fprintf(stderr, "usage: sudo <command> [args...]\n");
        return 2;
    }

    if (!permitted(ruid)) {
        fprintf(stderr, "sudo: uid %lu is not permitted\n", ruid);
        return 1;
    }

    for (fd = 3; fd < MAX_FD; fd++) {
        close(fd);
    }

    /* Assume root fully; the saved uid is already 0, so the real uid moves
     * too. */
    if (setresuid(0, 0, 0) != 0 || setresgid(0, 0, 0) != 0) {
        fprintf(stderr, "sudo: could not assume root\n");
        return 1;
    }

    execve(argv[1], &argv[1], clean_env);
    fprintf(stderr, "sudo: %s: cannot execute\n", argv[1]);
    return 1;
}
