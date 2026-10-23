/* image_fixture.c: file operations for the image product tests. */
#define _POSIX_C_SOURCE 200809L
#include "fixture.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mode_number(const char *text, mode_t *value)
{
    char *end;
    unsigned long n;
    errno = 0;
    n = strtoul(text, &end, 8);
    if (errno || !*text || *end || n > 0777)
        return 0;
    *value = (mode_t)n;
    return 1;
}

static int print_file(const char *path)
{
    unsigned char block[16384];
    size_t n;
    FILE *f = fopen(path, "rb");
    int ok = f != NULL;
    if (!f)
        return 0;
    while ((n = fread(block, 1, sizeof(block), f)) != 0)
        if (fwrite(block, 1, n, stdout) != n) {
            ok = 0;
            break;
        }
    if (ferror(f) || fflush(stdout))
        ok = 0;
    return fclose(f) == 0 && ok;
}

static int names_compare(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int print_directory(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    char **names = NULL, **grown;
    size_t n = 0, i;
    int ok = d != NULL;
    if (!d)
        return 0;
    errno = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        grown = realloc(names, (n + 1) * sizeof(*names));
        if (!grown) {
            ok = 0;
            break;
        }
        names = grown;
        names[n] = strdup(e->d_name);
        if (!names[n]) {
            ok = 0;
            break;
        }
        n++;
        errno = 0;
    }
    if (errno)
        ok = 0;
    if (closedir(d))
        ok = 0;
    if (n > 1)
        qsort(names, n, sizeof(*names), names_compare);
    for (i = 0; i < n; i++) {
        if (ok && puts(names[i]) == EOF)
            ok = 0;
        free(names[i]);
    }
    free(names);
    return !fflush(stdout) && ok;
}

static int make_file(const char *path, const char *text)
{
    char *end;
    unsigned long long n;
    int fd, ok;
    errno = 0;
    n = strtoull(text, &end, 10);
    if (errno || !*text || *end || n > 0x7fffffffffffffffULL)
        return 0;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 0;
    ok = ftruncate(fd, (off_t)n) == 0;
    return close(fd) == 0 && ok;
}

/** Prepare an overlay as the original builder's caller would under MASK. */
static int layer(const char *directory, const char *mask, const char *outside)
{
    static const char *const dirs[] = {"", "/usr", "/usr/libexec", "/usr/share",
                                       "/usr/share/probe"};
    char path[4096];
    mode_t bits;
    unsigned i;
    if (!mode_number(mask, &bits) || !fx_remove(directory))
        return 0;
    umask(bits);
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (snprintf(path, sizeof(path), "%s%s", directory, dirs[i]) >= (int)sizeof(path) ||
            mkdir(path, 0777))
            return 0;
    snprintf(path, sizeof(path), "%s/usr/libexec/probe", directory);
    if (!fx_write(path, "#!/bin/sh\n", 10, 0755))
        return 0;
    snprintf(path, sizeof(path), "%s/usr/share/probe/data", directory);
    if (!fx_write(path, "data\n", 5, 0666 & ~bits))
        return 0;
    snprintf(path, sizeof(path), "%s/usr/share/probe/outside", directory);
    return symlink(outside, path) == 0;
}

/** This executable is also the builder's overlay callback. */
static int overlay(const char *self, const char *target)
{
    char source[4096], scratch[4096], *layer_path;
    mode_t mask;
    int ok;
    if (snprintf(source, sizeof(source), "%s.layer", self) >= (int)sizeof(source))
        return 0;
    layer_path = fx_read(source);
    if (!layer_path || !*layer_path) {
        free(layer_path);
        return 0;
    }
    layer_path[strcspn(layer_path, "\r\n")] = 0;
    ok = fx_tree(layer_path, target);
    free(layer_path);
    if (snprintf(scratch, sizeof(scratch), "%s/tmp/scratch", target) >= (int)sizeof(scratch))
        return 0;
    mask = umask(0);
    umask(mask);
    return fx_write(scratch, "scratch\n", 8, 0666 & ~mask) && ok;
}

/** Execute one product with an explicit directory, umask and search path. */
static int run_product(int argc, char **argv)
{
    char path[8192];
    const char *previous = getenv("PATH");
    mode_t bits;
    if (argc < 7 || !mode_number(argv[3], &bits) || chdir(argv[2]))
        return 2;
    if (snprintf(path, sizeof(path), "%s:%s", argv[4], previous ? previous : "/usr/bin:/bin") >=
            (int)sizeof(path) ||
        setenv("PATH", path, 1))
        return 2;
    umask(bits);
    if (strcmp(argv[5], "merge") && strcmp(argv[5], "separate"))
        return 2;
    if (!strcmp(argv[5], "merge") && dup2(STDOUT_FILENO, STDERR_FILENO) < 0)
        return 2;
    execvp(argv[6], &argv[6]);
    perror(argv[6]);
    return 127;
}

static int one_path(const char *operation, const char *path)
{
    struct stat st;
    if (!strcmp(operation, "mkdir"))
        return fx_mkdir(path);
    if (!strcmp(operation, "reset"))
        return fx_remove(path) && fx_mkdir(path);
    if (!strcmp(operation, "remove"))
        return fx_remove(path) && lstat(path, &st) < 0 && errno == ENOENT;
    if (!strcmp(operation, "read"))
        return print_file(path);
    if (!strcmp(operation, "list"))
        return print_directory(path);
    if (!strcmp(operation, "absent"))
        return lstat(path, &st) < 0 && errno == ENOENT;
    if (!strcmp(operation, "mode") && !stat(path, &st))
        return printf("%o\n", (unsigned)(st.st_mode & 07777)) > 0;
    return 0;
}

static int two_paths(const char *operation, const char *a, const char *b)
{
    if (!strcmp(operation, "copy"))
        return fx_copy(a, b);
    if (!strcmp(operation, "link"))
        return symlink(a, b) == 0;
    if (!strcmp(operation, "truncate"))
        return make_file(a, b);
    if (!strcmp(operation, "chmod")) {
        mode_t bits;
        return mode_number(b, &bits) && chmod(a, bits) == 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    int ok = 0;
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "mke2fs") || !strcmp(name, "debugfs")) {
        fprintf(stderr, "injected failure in %s\n", name);
        return 1;
    }
    if (!strcmp(name, "overlay"))
        return argc == 2 && overlay(argv[0], argv[1]) ? 0 : 1;
    if (argc > 1 && !strcmp(argv[1], "run"))
        return run_product(argc, argv);
    if (argc == 3)
        ok = one_path(argv[1], argv[2]);
    else if (argc == 4)
        ok = two_paths(argv[1], argv[2], argv[3]);
    else if (argc == 5 && !strcmp(argv[1], "layer"))
        ok = layer(argv[2], argv[3], argv[4]);
    if (!ok)
        fprintf(stderr, "image-fixture: %s failed: %s\n", argc > 1 ? argv[1] : "operation",
                strerror(errno));
    return ok ? 0 : 1;
}
