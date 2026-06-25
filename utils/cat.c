/** Concatenate files (or stdin) to stdout. */

#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 4096
static char buffer[BUFFER_SIZE];

static void cat_fd(int fd)
{
    ssize_t n;

    while ((n = read(fd, buffer, BUFFER_SIZE)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(STDOUT_FILENO, buffer + off, (size_t)(n - off));
            if (w <= 0) {
                return;
            }
            off += w;
        }
    }
}

int main(int argc, char *argv[])
{
    int i;

    if (argc == 1) {
        cat_fd(STDIN_FILENO);
    } else {
        for (i = 1; i < argc; ++i) {
            int fd = open(argv[i], O_RDONLY);
            if (fd >= 0) {
                cat_fd(fd);
                close(fd);
            }
        }
    }

    return 0;
}
