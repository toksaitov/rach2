/** Print the current working directory. */

#include <stdio.h>
#include <unistd.h>

/**
 * Print the working directory and a newline. The status is 1, with no
 * message, when `getcwd` fails, as it does for a path of 256 bytes or more.
 */
int main(void)
{
    char cwd[256];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return 1;
    }
    puts(cwd);

    return 0;
}
