/**
 * cat: copy files, or standard input, to standard output through `read` and
 * `write`, without stdio.
 */

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#define BUFFER_SIZE 4096
static char buffer[BUFFER_SIZE];

/** Write `a` and then `b` to standard error, ignoring a failed write. */
static void say(const char *a, const char *b)
{
    write(STDERR_FILENO, a, strlen(a));
    write(STDERR_FILENO, b, strlen(b));
}

/**
 * Copy bytes from `fd` to standard output.
 *
 * Complete short writes. Report a write returning zero or an error and
 * return 1. Report a read error with the input name `path` and return 1.
 * Return 0 on EOF. Leave `fd` open.
 */
static int cat_fd(int fd, const char *path)
{
    ssize_t n;

    while ((n = read(fd, buffer, BUFFER_SIZE)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(STDOUT_FILENO, buffer + off, (size_t)(n - off));
            if (w <= 0) {
                say("cat: write error", "\n");
                return 1;
            }
            off += w;
        }
    }
    if (n < 0) {
        const char *error = strerror(errno);

        say("cat: ", path);
        say(": ", error);
        say("", "\n");
        return 1;
    }
    return 0;
}

/**
 * Copy named files to standard output, or standard input with no operands.
 *
 * Report an open failure and continue with later operands. Complete short
 * writes; report a write returning zero or an error and stop copying.
 * Report a read error and stop copying. Return 1 on these failures,
 * otherwise 0. Close errors do not affect the status.
 */
int main(int argc, char *argv[])
{
    int i;
    int status = 0;

    if (argc == 1) {
        return cat_fd(STDIN_FILENO, "standard input");
    }
    for (i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            say("cat: cannot open ", argv[i]);
            say("", "\n");
            status = 1;
            continue;
        }
        if (cat_fd(fd, argv[i]) != 0) {
            close(fd);
            return 1;
        }
        close(fd);
    }

    return status;
}
