/* family_image_mock.c: publication steps and a two-builder rendezvous. */
#define _POSIX_C_SOURCE 200809L
#include "family_image.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int step(int argc, char **argv, const char *name, const char *directory)
{
    char path[8192];
    unsigned n = 1;
    FILE *fp;
    for (;;) {
        snprintf(path, sizeof(path), "%s/%u", directory, n);
        if (!mkdir(path, 0700))
            break;
        if (errno != EEXIST)
            return 0;
        n++;
    }
    snprintf(path, sizeof(path), "%s/%u/command", directory, n);
    fp = fopen(path, "w");
    if (!fp)
        return 0;
    fprintf(fp, "%s", name);
    for (int i = 1; i < argc; i++)
        fprintf(fp, " %s", argv[i]);
    fputc('\n', fp);
    fclose(fp);
    if (getenv("IMAGERACE_STOP") && n == strtoul(getenv("IMAGERACE_STOP"), NULL, 10)) {
        char *pid;
        snprintf(path, sizeof(path), "%s/pid", directory);
        while (!fx_exists(path))
            fx_pause();
        pid = fx_read(path);
        if (strtol(pid, NULL, 10) <= 0 || kill((pid_t)strtol(pid, NULL, 10), SIGTERM)) {
            free(pid);
            return 0;
        }
        free(pid);
        snprintf(path, sizeof(path), "%s/%u/stopped", directory, n);
        fx_text(path, "");
    }
    return 1;
}
static int meet(const char *directory, const char *build)
{
    char ours[8192], other[8192], release[8192], met[8192];
    snprintf(ours, sizeof(ours), "%s/ready.%s", directory, build);
    snprintf(other, sizeof(other), "%s/ready.%s", directory, !strcmp(build, "a") ? "b" : "a");
    snprintf(release, sizeof(release), "%s/release", directory);
    snprintf(met, sizeof(met), "%s/met.%s", directory, build);
    if (!fx_text(ours, ""))
        return 0;
    while (!fx_exists(other) && !fx_exists(release))
        fx_pause();
    return !fx_exists(other) || fx_text(met, "");
}
int family_image_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/'), *image = getenv("IMAGERACE_OUT");
    char executable[4096];
    int affected = 0;
    name = name ? name + 1 : argv[0];
    for (int i = 1; image && i < argc; i++)
        affected |= strstr(argv[i], image) != NULL;
    if (affected && getenv("IMAGERACE_STEPS") && !step(argc, argv, name, getenv("IMAGERACE_STEPS")))
        return 2;
    if (affected && !strcmp(name, "touch") && getenv("IMAGERACE_MEET") &&
        !meet(getenv("IMAGERACE_MEET"), getenv("IMAGERACE_BUILD")))
        return 2;
    snprintf(executable, sizeof(executable), "/usr/bin/%s", name);
    execv(executable, argv);
    return 127;
}
