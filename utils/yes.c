/** Print a line forever: the joined arguments, or `y`. The loop ends when
    the output does: a write that fails, as when the reader has gone, is
    the one way out, and the status says so. */

#include <stdio.h>

int main(int argc, char *argv[])
{
    int i;

    if (argc > 1) {
        for (;;) {
            for (i = 1; i < argc; ++i) {
                if (fputs(argv[i], stdout) == EOF || putchar(i < argc - 1 ? ' ' : '\n') == EOF) {
                    return 1;
                }
            }
        }
    }
    for (;;) {
        if (fputs("y\n", stdout) == EOF) {
            return 1;
        }
    }
}
