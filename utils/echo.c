/*
 * echo.c - write the arguments separated by single spaces, then a newline.
 *
 * Ported to musl: ordinary <stdio.h>, no hand-rolled int-0x80 stubs or crt0.
 */

#include <stdio.h>

int main(int argc, char *argv[])
{
    int i;

    for (i = 1; i < argc; ++i) {
        fputs(argv[i], stdout);
        if (i < argc - 1) {
            putchar(' ');
        }
    }
    putchar('\n');

    return 0;
}
