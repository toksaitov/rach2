/* source_snapshot.c: a sorted source inventory with content digests. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sha256.h"
#include "source_snapshot.h"
#ifdef _WIN32
#include "win/files.h"
#endif

struct inventory {
    char **paths;
    size_t count;
    const char *tree;
};

static int join(char out[4096], const char *a, const char *b)
{
    int n = snprintf(out, 4096, "%s%s%s", a, *a ? "/" : "", b);
    return n < 0 || n >= 4096 ? -1 : 0;
}

static int root_directory(const char *name)
{
    static const char *const roots[] = {"rach2",  "krffs",   "utils",  "ish",     "tools",
                                        "tests",  "mk",      "man",    "dev-log", "ports",
                                        "vendor", ".github", ".agents"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(*roots); i++)
        if (!strcmp(name, roots[i]))
            return 1;
    return 0;
}

static int skip_directory(const char *parent, const char *name)
{
    static const char *const ignored[] = {"build",        "install",   "downloads", "__pycache__",
                                          "node_modules", "vs-dokany", ".cache"};
    if (!*parent)
        return !root_directory(name);
    if (name[0] == '.')
        return 1;
    if (!strcmp(name, "vendor"))
        return 1;
    for (size_t i = 0; i < sizeof(ignored) / sizeof(*ignored); i++)
        if (!strcmp(name, ignored[i]))
            return 1;
    if (!strncmp(parent, "vendor/", 7) && !strchr(parent + 7, '/')) {
        const char *component = parent + 7;
        size_t n = strlen(component);
        if (!strncmp(name, component, n) && name[n] == '-' && name[n + 1] >= '0' &&
            name[n + 1] <= '9')
            return 1;
    }
    return 0;
}

static int source_file(const char *name)
{
    static const char *const suffixes[] = {
        ".c",        ".h",      ".S",    ".s",   ".sh",      ".awk",   ".pl", ".py",    ".json",
        ".yml",      ".yaml",   ".md",   ".mk",  ".def",     ".inc",   ".in", ".patch", ".rules",
        ".manifest", ".cfg",    ".conf", ".txt", ".reasons", ".allow", ".ld", ".list",  ".pins",
        ".link",     ".script", ".1",    ".2",   ".3",       ".4",     ".5",  ".7",     ".8"};
    static const char *const names[] = {
        "Makefile", "Readme",        "LICENSE",    "COPYING",        "run",          "registry",
        "judges",   ".clang-format", ".gitignore", ".gitattributes", ".editorconfig"};
    size_t n = strlen(name);
    if (!strcmp(name, "compile_commands.json"))
        return 0;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(*suffixes); i++) {
        size_t tail = strlen(suffixes[i]);
        if (n >= tail && !strcmp(name + n - tail, suffixes[i]))
            return 1;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}

static int append(struct inventory *v, const char *path)
{
    char **next = realloc(v->paths, (v->count + 1) * sizeof(*v->paths));
    if (!next)
        return -1;
    v->paths = next;
    if (!(v->paths[v->count] = strdup(path)))
        return -1;
    v->count++;
    return 0;
}

static int scan(struct inventory *v, const char *relative)
{
    char directory[4096], child[4096], path[4096];
    struct dirent *entry;
    struct stat st;
    DIR *dir;
    int bad = 0;
    if (join(directory, v->tree, relative) || !(dir = opendir(directory)))
        return -1;
    errno = 0;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        if (join(child, relative, name) || join(path, v->tree, child) || lstat(path, &st)) {
            bad = 1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!skip_directory(relative, name) && scan(v, child)) {
                bad = 1;
                break;
            }
        } else if (S_ISREG(st.st_mode) && source_file(name)) {
            if (append(v, child)) {
                bad = 1;
                break;
            }
        }
        errno = 0;
    }
    if (errno)
        bad = 1;
    closedir(dir);
    return bad ? -1 : 0;
}

static int order(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int file_hash(const char *path, unsigned char digest[32])
{
    struct sha256 h;
    unsigned char block[16384];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_init(&h);
    while ((n = fread(block, 1, sizeof(block), f)))
        sha256_update(&h, block, n);
    int bad = ferror(f);
    if (fclose(f))
        bad = 1;
    sha256_final(&h, digest);
    return bad ? -1 : 0;
}

int source_snapshot(const char *tree, char hex[65])
{
    struct inventory v = {NULL, 0, tree};
    struct sha256 h;
    unsigned char digest[32], file[32];
    char path[4096];
    int bad = scan(&v, "");
    sha256_init(&h);
    qsort(v.paths, v.count, sizeof(*v.paths), order);
    for (size_t i = 0; i < v.count; i++) {
        if (!bad && (join(path, tree, v.paths[i]) || file_hash(path, file)))
            bad = 1;
        if (!bad) {
            sha256_update(&h, (const unsigned char *)v.paths[i], strlen(v.paths[i]) + 1);
            sha256_update(&h, file, sizeof(file));
        }
        free(v.paths[i]);
    }
    free(v.paths);
    sha256_final(&h, digest);
    sha256_hex(digest, hex);
    return bad ? -1 : 0;
}
