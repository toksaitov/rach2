/** yes: print a line over and over until the output fails. */

#include <stdio.h>

/**
 * Print the operands joined by single spaces, or `y` when there are none, as
 * one line, again and again, until a write fails; stdio finds the failure
 * when it writes out a full buffer, and the status is then 1. A pipe whose
 * reader has gone raises `SIGPIPE` on that write, which ends the program
 * first unless the signal is ignored.
 */
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
