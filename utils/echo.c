/** Write the arguments separated by single spaces, then a newline. */

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
