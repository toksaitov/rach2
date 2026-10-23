/* family_check.c: fixture ownership and the existing etude report format. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
static char paths[64][4096];
static unsigned next_path, records;
static const char *join(const char *root, const char *name)
{
    char *p = paths[next_path++ % 64];
    if (snprintf(p, 4096, "%s/%s", root, name) >= 4096)
        exit(2);
    return p;
}
const char *family_path(struct fixture *f, const char *name)
{
    return join(f->work, name);
}
const char *family_source(struct fixture *f, const char *name)
{
    return join(f->tree, name);
}
int family_row(struct fixture *f, const char *id, int held)
{
    printf("@etude 1 assert %s %s%s\n", id, held ? "pass" : "fail",
           held ? "" : " the product result differs; see the controller fixtures");
    fflush(stdout);
    records++;
    if (!held)
        f->failed = 1;
    return held;
}
int family_sparse(const char *path, uint64_t bytes)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600), ok;
    if (fd < 0)
        return 0;
    ok = ftruncate(fd, (off_t)bytes) == 0;
    return close(fd) == 0 && ok;
}
int family_digest(const char *path, char hex[65])
{
    FILE *fp = fopen(path, "rb");
    struct sha256 s;
    unsigned char data[65536], digest[32];
    size_t n;
    int ok;
    if (!fp)
        return 0;
    sha256_init(&s);
    while ((n = fread(data, 1, sizeof(data), fp)))
        sha256_update(&s, data, n);
    ok = !ferror(fp);
    fclose(fp);
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
    return ok;
}
int family_extract(struct fixture *f, const char *names)
{
    char *text = fx_read(family_source(f, "tools/dev/machine_grow.sh")), *line, *next;
    FILE *out = fopen(family_path(f, "functions.sh"), "wb");
    int copying = 0, found = 0;
    if (!text || !*text || !out) {
        free(text);
        if (out)
            fclose(out);
        return 0;
    }
    fputs("fail() { echo \"grow: failed: $*\" >&2; exit 1; }\n", out);
    for (line = text; *line; line = next) {
        char name[128], token[132];
        size_t n;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        else
            next = line + strlen(line);
        if (sscanf(line, "%127[A-Za-z_]", name) == 1) {
            n = strlen(name);
            snprintf(token, sizeof(token), "|%s|", name);
            if (!strcmp(line + n, "()") && strstr(names, token)) {
                copying = 1;
                found++;
            }
        }
        if (copying) {
            const char *at = line, *hit;
            while ((hit = strstr(at, "/bin/grow"))) {
                fwrite(at, 1, (size_t)(hit - at), out);
                fputs("/host-products/grow", out);
                at = hit + strlen("/bin/grow");
            }
            fputs(at, out);
            fputc('\n', out);
            if (!strcmp(line, "}"))
                copying = 0;
        }
    }
    free(text);
    return fclose(out) == 0 && found;
}
int family_run(struct fixture *f, const char *const argv[], const char *cwd,
               const char *const env[], const char *input)
{
    return fx_run(f, argv, cwd, env, input) == 0;
}
int family_eval(struct fixture *f, const char *command)
{
    char source[16384];
    const char *a[] = {"/bin/sh", "-eu", "-c", source, NULL};
    if (snprintf(source, sizeof(source), ". \"$W/functions.sh\"\n%s", command) >=
        (int)sizeof(source))
        return 0;
    return family_run(f, a, f->work, NULL, NULL);
}
int family_seed(struct fixture *f, const char *name, unsigned mib, const char *group)
{
    const char *file = family_path(f, name);
    const char *a[] = {
        "/host-products/mke2fs",
        "-q",
        "-F",
        "-t",
        "ext2",
        "-b",
        "4096",
        "-g",
        group,
        "-I",
        "128",
        "-O",
        "sparse_super,large_file,filetype,resize_inode,^dir_index,^ext_attr,^has_journal,^extent,^"
        "64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize",
        file,
        NULL};
    return family_sparse(file, (uint64_t)mib * 1048576) && family_run(f, a, f->work, NULL, NULL);
}
int main(int argc, char **argv)
{
    struct fixture f;
    const char *name = strrchr(argv[0], '/');
    char path[8192];
    int ok;
    name = name ? name + 1 : argv[0];
    if (strcmp(name, "host-family-check"))
        return family_grow_mock(argc, argv);
    if (argc != 3 || !fx_init(&f, argv[2], argv[1], argv[0]))
        return 2;
    snprintf(path, sizeof(path), "%s:/host-products:/usr/bin:/bin:/usr/sbin:/sbin", f.bin);
    fx_env("PATH", path);
    fx_env("W", f.work);
    fx_env("LC_ALL", "C");
    fx_env("E2FSPROGS_FAKE_TIME", "1");
    fx_env("TMPDIR", family_path(&f, "tmp"));
    fx_mkdir(getenv("TMPDIR"));
    printf("@etude 1 begin %s\n", argv[1]);
    fflush(stdout);
    ok = family_grow(&f, argv[1]);
    if (!ok)
        f.failed = 1;
    printf("@etude 1 end %u\n", records);
    ok = !f.failed;
    fx_destroy(&f);
    return ok ? 0 : 1;
}
