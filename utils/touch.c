/** touch: create each named file if absent and set its timestamps to now. */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * Open each operand to create it, then set its access and modification times
 * to the current time with `utimensat` and a NULL `times`.
 *
 * The open is for the creation alone, with mode 0666 less the umask, and its
 * failure is ignored, so a directory or a file the caller cannot write still
 * has its times set when the caller may set them. `O_NONBLOCK` keeps the open
 * of a FIFO from waiting for a reader, and `O_NOCTTY` keeps a terminal from
 * becoming the controlling one. A file whose times cannot be set is reported
 * and the rest are still tried; the status is 1 when any failed, or on a
 * usage error.
 */
int main(int argc, char *argv[])
{
    int i;
    int failed = 0;

    if (argc < 2) {
        fputs("Usage: touch <file> [...]\n", stderr);
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | O_NOCTTY | O_NONBLOCK, 0666);
        if (fd >= 0) {
            close(fd);
        }
        if (utimensat(AT_FDCWD, argv[i], NULL, 0) != 0) {
            fprintf(stderr, "touch: cannot touch '%s'\n", argv[i]);
            failed = 1;
        }
    }

    return failed;
}
