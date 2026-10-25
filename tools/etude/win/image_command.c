/* image_command.c: native entry points for local tooling image commands. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hold.h"

/** Run the command named by this executable through the collection adapter. */
int main(int argc, char **argv)
{
    char self[4096], root[4096], program[4096], *part, *name;
    char **args;
    int i, n;

    n = (int)GetModuleFileNameA(NULL, self, sizeof(self));
    if (n <= 0 || n >= (int)sizeof(self))
        return 2;
    for (i = 0; self[i]; i++)
        if (self[i] == '\\')
            self[i] = '/';
    name = strrchr(self, '/');
    if (name == NULL)
        return 2;
    name++;
    part = strrchr(name, '.');
    if (part != NULL)
        *part = '\0';
    strcpy(root, self);
    for (i = 0; i < 5; i++) {
        part = strrchr(root, '/');
        if (part == NULL)
            return 2;
        *part = '\0';
    }
    if (snprintf(program, sizeof(program), "%s/build/tools/etude/etude", root) >=
        (int)sizeof(program))
        return 2;
    args = calloc((size_t)argc + 9, sizeof(*args));
    if (args == NULL)
        return 2;
    args[0] = program;
    args[1] = "prepare";
    args[2] = "tools";
    args[3] = "tool";
    args[4] = root;
    args[5] = name;
    for (i = 1; i < argc; i++)
        args[i + 5] = argv[i];
    hold_exec(args);
    free(args);
    perror("image-command");
    return 2;
}
