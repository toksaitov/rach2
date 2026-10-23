/* family_grow_mock.c: disk faults and measured-output producers in C. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
static int bootctl(char **argv, const char *w)
{
    char path[4096];

    int in, out, ok = 1;
    unsigned char buffer[65536];
    uint64_t left = 67108864;
    snprintf(path, sizeof(path), "%s/diskp1", w);
    if (strcmp(argv[2], path)) {
        execv("/host-products/bootctl", argv);
        return 127;
    }
    snprintf(path, sizeof(path), "%s/disk", w);
    in = open(path, O_RDONLY);
    snprintf(path, sizeof(path), "%s/current-boot.img", w);
    out = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (in < 0 || out < 0 || lseek(in, 1048576, SEEK_SET) < 0)
        ok = 0;
    while (ok && left) {
        size_t n = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
        if (read(in, buffer, n) != (ssize_t)n || write(out, buffer, n) != (ssize_t)n)
            ok = 0;
        left -= n;
    }
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    if (!ok)
        return 1;
    argv[2] = path;
    execv("/host-products/bootctl", argv);
    return 127;
}
static int reflash(int argc, char **argv, const char *w)
{
    char path[4096];
    FILE *fp;

    uint64_t at, length;
    int fd, tear = getenv("GROW_BOOT_TEAR") != NULL, ok;
    unsigned char data[1025];
    if (argc > 1 && !strcmp(argv[1], "--verify")) {
        execv("/host-products/reflash", argv);
        return 127;
    }
    snprintf(path, sizeof(path), "%s/disk", w);
    if (argc != 4 || strcmp(path, argv[1]))
        return 2;
    at = strtoull(argv[2], NULL, 10);
    length = strtoull(argv[3], NULL, 10);
    if (at < 1048576 || at + length > 68157440 || length != 1024 ||
        fread(data, 1, (size_t)length + 1, stdin) != length)
        return 2;
    length = tear ? 512 : length;
    snprintf(path, sizeof(path), "%s/writes", w);
    fp = fopen(path, "a");
    if (!fp)
        return 1;
    fprintf(fp, "%llu %llu\n", (unsigned long long)at, (unsigned long long)length);
    fclose(fp);
    fd = open(argv[1], O_WRONLY);
    ok =
        fd >= 0 && pwrite(fd, data, (size_t)length, (off_t)at) == (ssize_t)length && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    return ok && !tear ? 0 : 1;
}
static int output(const char *name, const char *mode)
{
    unsigned i;
    if (!strcmp(name, "growroot")) {
        if (!mode)
            return 2;
        if (!strcmp(mode, "overflow")) {
            for (i = 0; i < 8192; i++)
                puts("too much output");
            return 0;
        }
        puts(!strcmp(mode, "measure-fail") ? "measured failure" : "measured output");
        return !strcmp(mode, "measure-fail") ? 23 : 0;
    }
    if (!strcmp(name, "producer")) {
        if (!mode)
            return 2;
        if (!strcmp(mode, "log")) {
            for (i = 0; i < 4096; i++)
                puts("restore completion");
            return 0;
        }
        puts("producer output");
        return 23;
    }
    return 2;
}
int family_grow_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/'), *w = getenv("W"), *mode = getenv("GROW_MODE");
    char path[4096];
    FILE *fp;
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "shasum") && argc == 4 && !strcmp(argv[1], "-a") && !strcmp(argv[2], "256")) {
        char hex[65];
        if (!family_digest(argv[3], hex))
            return 1;
        printf("%s  %s\n", hex, argv[3]);
        return 0;
    }
    if (!w)
        return 2;
    if (!strcmp(name, "mount") || !strcmp(name, "umount")) {
        snprintf(path, sizeof(path), "%s/transitions", w);
        fp = fopen(path, "a");
        if (!fp)
            return 1;
        fputs(!strcmp(name, "mount") ? "mounted\n" : "unmounted\n", fp);
        return fclose(fp) ? 1 : 0;
    }
    if (!strcmp(name, "growroot") || !strcmp(name, "producer"))
        return output(name, mode);
    if (!strcmp(name, "bootctl") && argc >= 4)
        return bootctl(argv, w);
    if (!strcmp(name, "reflash"))
        return reflash(argc, argv, w);
    return 2;
}
