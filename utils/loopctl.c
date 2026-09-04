/**
 * loopctl: attach a file to a loop device, detach one, or list them.
 *
 * `loopctl attach FILE` takes the first free device, `loopctl attach N FILE`
 * takes that one, `loopctl attach -r [N] FILE` takes one that will not write,
 * and `loopctl detach N` gives it back. With no arguments it prints what
 * /proc/loop says: the device, its size in bytes, whether it takes writes,
 * and the file it was attached by.
 *
 * The kernel is spoken to through /dev/loop-control, one command per write,
 * which is what a shell can send and what a program of this size can afford.
 * The device is what `mount(2)` then takes as its source, so a file system in
 * a file is mounted like any other volume.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
            snprintf(line, sizeof(line), "attach %s- %s", ro, argv[at]);
            return command(line);
        }
        if (argc == at + 2) {
            snprintf(line, sizeof(line), "attach %s%s %s", ro, argv[at], argv[at + 1]);
            return command(line);
        }
    }
    if (argc == 3 && strcmp(argv[1], "detach") == 0) {
        snprintf(line, sizeof(line), "detach %s", argv[2]);
        return command(line);
    }
    fputs("usage: loopctl [attach [-r] [N] FILE | detach N]\n", stderr);
    return 2;
}
