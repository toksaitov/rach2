/**
 * Concatenate files (or stdin) to stdout.
 *
 * A write that fails is reported and ends the program with status 1: a
 * redirect into a device that refuses the bytes must say so, or a caller
 * reads silence as success. An operand that cannot be opened is reported the
 * same way and the rest are still copied.
 */

#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define BUFFER_SIZE 4096
static char buffer[BUFFER_SIZE];

static void say(const char *a, const char *b)
{
    write(STDERR_FILENO, a, strlen(a));
    write(STDERR_FILENO, b, strlen(b));
}

/* Copy fd to stdout; 0 when everything was written, 1 on a write error. */
static int cat_fd(int fd)
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
    return 0;
}

int main(int argc, char *argv[])
{
    int i;
    int status = 0;

    if (argc == 1) {
        return cat_fd(STDIN_FILENO);
    }
    for (i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            say("cat: cannot open ", argv[i]);
            say("", "\n");
            status = 1;
            continue;
        }
        if (cat_fd(fd) != 0) {
            close(fd);
            return 1;
        }
        close(fd);
    }

    return status;
}
