/*
 * pwd.c - print the current working directory.
 *
 * Ported to musl: <unistd.h> getcwd + <stdio.h> puts, no int-0x80 stubs.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char cwd[256];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return 1;
    }
    puts(cwd);

    return 0;
}
