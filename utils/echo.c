/** Write the arguments separated by single spaces, then a newline. */

#include <stdio.h>

/**
 * Write the operands. No option is recognized and no escape is interpreted,
 * so `-n` is written as an operand. The status is 0, even when the write
 * fails.
 */
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
