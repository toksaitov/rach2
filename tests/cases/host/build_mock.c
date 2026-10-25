/* Native command substitutes for build-product behavior tests. */
#include "build_support.h"
#include <time.h>

int build_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "uname")) {
        puts(getenv("UNAME_S") ? getenv("UNAME_S") : "Linux");
        return 0;
    }
    if (!strcmp(name, "qemu-system-i386") || !strcmp(name, "qemu-system-x86_64")) {
        const char *path = getenv("ARGS_OUT");
        FILE *f = path ? fopen(path, "w") : NULL;
        if (!f)
            return 90;
        if (!strcmp(name, "qemu-system-x86_64"))
            fprintf(f, "%s\n", name);
        for (int i = 1; i < argc; i++)
            fprintf(f, "%s\n", argv[i]);
        return fclose(f) ? 91 : 0;
    }
    if (!strcmp(name, "x86_64-elf-gdb")) {
        while (!fx_exists(getenv("ARGS_OUT")))
            fx_pause();
        return 0;
    }
    if (!strcmp(name, "cc")) {
        puts("x86_64-linux-gnu");
        return 0;
    }
    if (strstr(name, "i686-linux-gnu-"))
        return 0;
    if (!strcmp(name, "nproc") || !strcmp(name, "getconf")) {
        puts("32");
        return 0;
    }
    if (!strcmp(name, "bootstrap")) {
        const char *parallel = NULL;
        char jobs[64];
        for (int i = 1; i < argc; i++)
            if (!strncmp(argv[i], "--parallel=", 11))
                parallel = argv[i] + 11;
        if (!fx_text("Makefile", "all:\ninstall:\n"))
            return 92;
        snprintf(jobs, sizeof(jobs), "-j%s", parallel ? parallel : "");
        const char *args[] = {"/usr/bin/make",        "-s", "-f", getenv("JOBS_MK"),
                              parallel ? jobs : NULL, NULL};
        execvp(args[0], (char *const *)args);
        return 93;
    }
    return -1;
}
