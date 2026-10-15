/** cp: copy the bytes of one file to a path. */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define BUFFER_SIZE 4096
static char buffer[BUFFER_SIZE];

/**
 * Copy one source file to one destination path.
 *
 * Exactly two operands are taken. The destination is opened and truncated
 * before anything is read, and nothing checks that the two paths name
 * different files, so copying a file onto itself empties it. A missing
 * destination is created with mode 0666 less the umask.
 *
 * The status is 1 for a usage error, a file that cannot be opened, or a
 * failed read or write. A read error is reported with the source path.
 * A failed close does not change the status.
 */
int main(int argc, char *argv[])
{
    int fd_src, fd_dest;
    int status = 0;
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
    if (n < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        status = 1;
    }
    close(fd_src);
    close(fd_dest);

    return status;
}
