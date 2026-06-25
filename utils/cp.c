/** Copy a file from SOURCE to DEST. */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 4096
static char buffer[BUFFER_SIZE];

int main(int argc, char *argv[])
{
    int fd_src, fd_dest;
    ssize_t n;

    if (argc != 3) {
        fputs("Usage: cp <source> <destination>\n", stderr);
        return 1;
    }

    fd_src = open(argv[1], O_RDONLY);
    if (fd_src < 0) {
        fputs("cp: cannot open the source file\n", stderr);
        return 1;
    }
    fd_dest = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_dest < 0) {
        fputs("cp: cannot create the destination file\n", stderr);
        close(fd_src);
        return 1;
    }

    while ((n = read(fd_src, buffer, BUFFER_SIZE)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd_dest, buffer + off, (size_t)(n - off));
            if (w <= 0) {
                fputs("cp: write error\n", stderr);
                close(fd_src);
                close(fd_dest);
                return 1;
            }
            off += w;
        }
    }
    close(fd_src);
    close(fd_dest);

    return 0;
}
