/** Print the current working directory. */

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
