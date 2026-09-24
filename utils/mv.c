/**
 * Rename a file from its source path to its destination path.
 *
 * It is one `rename`: a destination on another mount is refused with
 * `EXDEV`, and nothing is copied.
 */

#include <stdio.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fputs("Usage: mv <source> <destination>\n", stderr);
        return 1;
    }

    if (rename(argv[1], argv[2]) < 0) {
        fputs("mv: cannot move file\n", stderr);
        return 1;
    }

    return 0;
}
