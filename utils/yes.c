/** Print a line forever: the joined arguments, or `y`. */

#include <stdio.h>

int main(int argc, char *argv[])
{
    int i;

    if (argc > 1) {
        for (;;) {
            for (i = 1; i < argc; ++i) {
                fputs(argv[i], stdout);
                putchar(i < argc - 1 ? ' ' : '\n');
            }
        }
    } else {
        for (;;) {
            fputs("y\n", stdout);
        }
    }

    return 0;
}
