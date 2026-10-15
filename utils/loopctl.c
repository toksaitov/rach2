/**
 * loopctl: attach a file to a loop device, detach one, or list them.
 *
 * `loopctl attach FILE` takes the first free device, `loopctl attach N FILE`
 * takes that one, `loopctl attach -r [N] FILE` takes one that will not write,
 * and `loopctl detach N` gives it back. With no arguments it prints what
 * /proc/loop says: the device, its size in bytes, whether it takes writes,
 * how many writes and syncs have reached the file, and the file it was
 * attached by, or `(gone)`.
 *
 * The kernel is spoken to through /dev/loop-control, one command per write,
 * which is what a shell can send and what a program of this size can afford.
 * The device is what `mount(2)` then takes as its source, so a file system in
 * a file is mounted like any other volume. The kernel takes the file by an
 * absolute path, so a relative one is resolved here first, as losetup takes
 * one on Linux.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Copy /proc/loop to standard output. Returns 0, or 1 on a failure. */
static int list(void)
{
    char buffer[512];
    int fd = open("/proc/loop", O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        perror("loopctl: /proc/loop");
        return 1;
    }
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        if (write(1, buffer, (size_t)n) != n) {
            close(fd);
            return 1;
        }
    }
    close(fd);
    return n < 0 ? 1 : 0;
}

/**
 * Send `line` to /dev/loop-control as one write. Returns 0, or 1 when the node
 * cannot be opened or the write of the command fails.
 */
static int command(const char *line)
{
    int fd = open("/dev/loop-control", O_WRONLY);
    ssize_t n;

    if (fd < 0) {
        perror("loopctl: /dev/loop-control");
        return 1;
    }
    n = write(fd, line, strlen(line));
    if (n < 0) {
        perror("loopctl");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

/**
 * Send the attach command for `file`, of device `n` ("-" for the first free
 * one), read-only when `ro` is "ro ". Returns 0, or 1 after a diagnostic.
 */
static int attach(const char *ro, const char *n, const char *file)
{
    char path[PATH_MAX];
    char line[PATH_MAX + 32];

    if (!realpath(file, path)) {
        fprintf(stderr, "loopctl: %s: %s\n", file, strerror(errno));
        return 1;
    }
    if (snprintf(line, sizeof(line), "attach %s%s %s", ro, n, path) >= (int)sizeof(line)) {
        fprintf(stderr, "loopctl: %s: the path is too long\n", file);
        return 1;
    }
    return command(line);
}

/** Run the command the arguments name. Exits 0, 1 on a failure and 2 on a
 *  usage error. */
int main(int argc, char **argv)
{
    char line[160];

    if (argc == 1) {
        return list();
    }
    if (argc >= 3 && strcmp(argv[1], "attach") == 0) {
        const char *ro = "";
        int at = 2;

        if (strcmp(argv[2], "-r") == 0) {
            ro = "ro ";
            at = 3;
        }
        if (argc == at + 1) {
            return attach(ro, "-", argv[at]);
        }
        if (argc == at + 2) {
            return attach(ro, argv[at], argv[at + 1]);
        }
    }
    if (argc == 3 && strcmp(argv[1], "detach") == 0) {
        snprintf(line, sizeof(line), "detach %s", argv[2]);
        return command(line);
    }
    fputs("usage: loopctl [attach [-r] [N] FILE | detach N]\n", stderr);
    return 2;
}
