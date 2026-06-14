/*
 * mv.c - rename a file (source -> destination).
 *
 * Ported to musl: <stdio.h> rename(), no int-0x80 stubs.
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
