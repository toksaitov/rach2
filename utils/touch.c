/*
 * touch.c - create each file if absent and bump its timestamps to now.
 *
 * Ported to musl. Note: this kernel's utimensat rejects the NULL-pathname form
 * (so futimens(fd, NULL) is a no-op), so we name the file: utimensat with a NULL
 * `times` means "set both times to the current time".
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
        /* Create the file if it does not exist yet. */
        int fd = open(argv[i], O_WRONLY | O_CREAT | O_NOCTTY | O_NONBLOCK, 0666);
        if (fd >= 0) {
            close(fd);
        }
        /* Set both timestamps to now (NULL times = current time). */
        if (utimensat(AT_FDCWD, argv[i], NULL, 0) != 0) {
            fprintf(stderr, "touch: cannot touch '%s'\n", argv[i]);
            failed = 1;
        }
    }

    return failed;
}
