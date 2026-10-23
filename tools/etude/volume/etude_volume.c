/*
 * etude_volume.c: etude-volume, the helper that makes the tooling machine's
 * seed and job volumes and reads the job volume back, which `etude provision
 * tooling` runs from its own directory. tools/etude/Readme.md gives its
 * usage.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "volume.h"

static int usage(void)
{
    fprintf(stderr, "usage: etude-volume seed IMG LABEL DIR NAME...\n"
                    "       etude-volume job IMG MIB [-d NAME | NAME PATH]...\n"
                    "       etude-volume read IMG NAME PATH [NAME PATH]...\n");
    return 2;
}

/**
 * Make job volume `img` of `mib` MiB from the operands `argv`: `-d NAME`
 * adds an empty directory, and `NAME PATH` the host file or directory PATH.
 */
static int job(const char *img, const char *mib, int argc, char **argv, char *why, size_t whylen)
{
    struct etude_volume *v;
    char *end;
    unsigned long n = strtoul(mib, &end, 10);
    int i;

    if (end == mib || *end != '\0' || n == 0 || n > 1048576ul)
        return usage();
    for (i = 0; i < argc; i += 2)
        if (i + 1 == argc || (argv[i][0] == '-' && strcmp(argv[i], "-d") != 0))
            return usage();
    if ((v = etude_volume_create(img, (uint64_t)n << 20, why, whylen)) == NULL)
        return 1;
    for (i = 0; i < argc; i += 2)
        if (strcmp(argv[i], "-d") == 0)
            etude_volume_dir(v, argv[i + 1]);
        else
            etude_volume_add(v, argv[i], argv[i + 1]);
    return etude_volume_close(v) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    char why[1024];
    int rc;

    why[0] = '\0';
    if (argc >= 6 && strcmp(argv[1], "seed") == 0)
        rc = etude_volume_seed(argv[2], argv[3], argv[4], (const char *const *)argv + 5, why,
                               sizeof(why)) == 0
                 ? 0
                 : 1;
    else if (argc >= 4 && strcmp(argv[1], "job") == 0)
        rc = job(argv[2], argv[3], argc - 4, argv + 4, why, sizeof(why));
    else if (argc >= 5 && argc % 2 == 1 && strcmp(argv[1], "read") == 0)
        rc = etude_volume_read(argv[2], (const char *const *)argv + 3, why, sizeof(why)) == 0 ? 0
                                                                                              : 1;
    else
        return usage();
    if (rc == 1)
        fprintf(stderr, "etude-volume: %s\n", why);
    return rc;
}
