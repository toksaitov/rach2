/**
 * sudo: run a command as root, if the real user is permitted.
 *
 * This is Rach2's reference sudo, the answer to a coursework exercise: with
 * the kernel's set-uid exec, a working sudo is about a hundred lines. Only the
 * test image stages it, for `sudoprobe`; the other images stage `su` and
 * leave sudo for the student to write.
 *
 * The program runs setuid root (the exec gave it effective uid 0 while the real
 * uid is still the caller's). It reads `/etc/sudoers`, a list of the numeric
 * uids allowed to use it, one per line; any other line, a `#` comment among
 * them, permits nobody. The check is of the real uid, which the caller cannot
 * change, and not of the effective uid the set-uid bit gave. A permitted
 * caller gets a fixed environment, so that no variable of the caller's, `PATH`
 * among them, can steer a root program, root as all three uids and gids, and
 * the command; anyone else gets an error and status 1.
 *
 * Usage: `sudo <command> [args...]`, where `<command>` is a path: it is not
 * looked up in `PATH`. Left out for the exercise to add: per-command rules, a
 * password prompt, and terminal handling.
 */

#define _GNU_SOURCE /* setresuid */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define SUDOERS "/etc/sudoers"

/* Descriptors on Rach2 are 0 to 1023 (`FD_TABLE_LIMIT`, rach2/sys/fs/fd.h). */
#define MAX_FD 1024

/* A fixed, minimal environment for the command: never the caller's, which a
 * setuid program cannot trust. */
static char *const clean_env[] = {(char *)"PATH=/bin:/usr/bin", (char *)"HOME=/root", NULL};

/**
 * Read one line of `f` into `line`, of `size` bytes, without its newline.
 * Returns its length, -1 at the end or on a read error, or -2 for a line it
 * does not accept, one with a NUL byte or longer than `size - 1`, which it
 * reads to its end.
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
 * Return 1 when a complete line of /etc/sudoers names `uid` and nothing else,
 * and 0 otherwise.
 *
 * Blank lines and comments are skipped, and a line `sudoers_line` does not
 * accept is skipped whole rather than read in part. A missing or unreadable
 * file permits no one.
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
 * Run `argv[1]` as root with the arguments after it, if the real uid is listed
 * in `/etc/sudoers`.
 *
 * Every descriptor above standard error is closed before the identity changes.
 * Exits 2 for a usage error and 1 for a caller not listed or a failure to
 * become root or to exec; on success the command runs in the program's place.
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

    /* Effective uid 0 may set all three ids, the real one included. */
    if (setresuid(0, 0, 0) != 0 || setresgid(0, 0, 0) != 0) {
        fprintf(stderr, "sudo: could not assume root\n");
        return 1;
    }

    execve(argv[1], &argv[1], clean_env);
    fprintf(stderr, "sudo: %s: cannot execute\n", argv[1]);
    return 1;
}
