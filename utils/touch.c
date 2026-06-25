/**
 * Create each named file if absent and set its timestamps to now.
 *
 * This kernel's `utimensat` rejects the NULL-pathname form (so
 * `futimens(fd, NULL)` is a no-op), so each file is named: `utimensat` with a
 * NULL `times` sets both times to the current time.
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
