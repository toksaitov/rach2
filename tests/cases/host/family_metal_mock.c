/* family_metal_mock.c: native stand-ins for deployment subprocesses. */
#define _POSIX_C_SOURCE 200809L
#include "family_metal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int lines(const char *path)
{
    char *s = fx_read(path), *p;
    int n = 0;
    for (p = s; p && *p; p++)
        n += *p == '\n';
    free(s);
    return n;
}
static int audit_rm(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[argc - 1] : "", *audit = getenv("PTY_SECRET_AUDIT");
    if (strstr(directory, "/rach2-accept-su.") && audit) {
        static const char *names[] = {"raw", "text"};
        unsigned i;
        for (i = 0; i < 2; i++) {
            char src[8192], dst[8192];
            snprintf(src, sizeof(src), "%s/%s", directory, names[i]);
            snprintf(dst, sizeof(dst), "%s/%s", audit, names[i]);
            if (fx_exists(src) && !fx_copy(src, dst))
                return 1;
        }
    }
    execv("/bin/rm", argv);
    return 127;
}
static int transport_sh(int argc, char **argv)
{
    if (argc >= 2 && strstr(argv[1], "/su_command.sh")) {
        const char *a[] = {"ssh", argc > 7 ? argv[7] : "", NULL};
        execvp(a[0], (char *const *)a);
        return 127;
    }
    if (argc >= 4 && strstr(argv[1], "/mkimage.sh"))
        return !strcmp(getenv("ACCEPT_CASE"), "image-fail") || !fx_text(argv[3], "image");
    execv("/bin/sh", argv);
    return 127;
}
static int mcopy(int argc, char **argv)
{
    char path[8192];
    const char *w = getenv("W");
    FILE *out;
    if (argc < 2 || !w)
        return 2;
    snprintf(path, sizeof(path), "%s/host-read", w);
    if (!fx_text(path, ""))
        return 1;
    snprintf(path, sizeof(path), "%s/payload", w);
    if (!fx_copy(path, argv[argc - 1]))
        return 1;
    if (strcmp(getenv("ACCEPT_CASE"), "fat-host"))
        return 0;
    out = fopen(argv[argc - 1], "r+b");
    if (!out)
        return 1;
    fputc('x', out);
    return fclose(out) ? 1 : 0;
}
int family_metal_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/'), *mode = getenv("FAMILY_METAL");
    name = name ? name + 1 : argv[0];
    if (!mode)
        return 2;
    if (!strcmp(mode, "password")) {
        if (!strcmp(name, "su") && argc == 3 && !strcmp(argv[1], "-c")) {
            execl("/bin/sh", "sh", "-c", argv[2], (char *)NULL);
            return 127;
        }
        if (!strcmp(name, "rm"))
            return audit_rm(argc, argv);
        if (!strcmp(name, "after_read") && argc == 2 && getenv("PTYDRIVE_READS")) {
            int before = lines(getenv("PTYDRIVE_READS"));
            fputs(argv[1], stdout);
            fflush(stdout);
            while (lines(getenv("PTYDRIVE_READS")) <= before)
                fx_pause();
            return 0;
        }
        return 2;
    }
    if (!strcmp(name, "ssh") && argc > 1)
        return family_metal_ssh(argv[argc - 1]);
    if (!strcmp(name, "sh"))
        return transport_sh(argc, argv);
    if (!strcmp(name, "mcopy"))
        return mcopy(argc, argv);
    if (!strcmp(name, "mkfs.krffs") || !strcmp(name, "fsck.krffs"))
        return 0;
    if (!strcmp(name, "readfile.krffs") && argc == 4)
        return fx_text(argv[3], "written-on-the-machine\n") ? 0 : 1;
    return 2;
}
