/* host_image_krffs.c: native KRFFS tools, with an independent ring reader. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "host_image.h"

struct ring {
    char names[512];
    uint64_t beta;
    int valid;
};

/** Decode the format independently of the tools' packed structures. */
static uint64_t little64(const unsigned char *p)
{
    unsigned i;
    uint64_t n = 0;
    for (i = 0; i < 8; i++)
        n |= (uint64_t)p[i] << (8 * i);
    return n;
}

static struct ring read_ring(const char *path, long base, size_t size)
{
    struct ring r;
    unsigned char node[357];
    uint64_t at = 0, length;
    unsigned count;
    FILE *f = fopen(path, "rb");
    memset(&r, 0, sizeof(r));
    for (count = 0; f != NULL && count < 64 && at < size; count++) {
        const char *name;
        if (fseek(f, base + (long)at, SEEK_SET) || fread(node, 1, sizeof(node), f) != sizeof(node))
            break;
        length = little64(node + 325);
        node[266] = '\0';
        name = node[2] == 0 ? "root" : node[2] == 1 ? "free" : (char *)node + 11;
        if (strlen(r.names) + strlen(name) + 2 >= sizeof(r.names) || length < sizeof(node) ||
            length > size - at || node[2] > 2)
            break;
        if (count != 0)
            strcat(r.names, " ");
        strcat(r.names, name);
        if (strcmp(name, "beta") == 0)
            r.beta = at;
        at += length;
    }
    r.valid = at == size;
    if (f != NULL)
        fclose(f);
    return r;
}

/** Give one private file of the native tools. */
static int path(struct image_check *t, const char *name, char out[4096])
{
    char evidence[160];
    snprintf(evidence, sizeof(evidence), "krffs-%s", name);
    return net_path(&t->u, evidence, out, 4096) != NULL;
}

static int file_size(const char *file, size_t size)
{
    unsigned char zeros[65536] = {0};
    FILE *f = fopen(file, "wb");
    int ok = f != NULL;
    while (ok && size != 0) {
        size_t n = size < sizeof(zeros) ? size : sizeof(zeros);
        ok = fwrite(zeros, 1, n, f) == n;
        size -= n;
    }
    return f != NULL && fclose(f) == 0 && ok;
}

static int native(struct image_check *t, const char *tool, char *const args[], struct net_run *r)
{
    char artifact[80], output[128];
    char *argv[24];
    const char *program;
    unsigned n;
    snprintf(artifact, sizeof(artifact), "host-krffs-%s", tool);
    memset(r, 0, sizeof(*r));
    program = etude_artifact(t->u.c, artifact);
    if (program == NULL)
        return 0;
    argv[0] = artifact;
    for (n = 0; args[n] != NULL && n + 2 < sizeof(argv) / sizeof(argv[0]); n++)
        argv[n + 1] = args[n];
    argv[n + 1] = NULL;
    snprintf(output, sizeof(output), "krffs-%s-%u", tool, ++t->commands);
    return net_host(&t->u, argv, output, r) == ETUDE_OK;
}

static int format(struct image_check *t, const char *file, char *const region[])
{
    struct net_run r = {0};
    char *args[] = {(char *)file, region[0], region[1], region[2], region[3], NULL};
    int ok = native(t, "mkfs", args, &r) && net_exited0(&r);
    net_run_free(&r);
    return ok;
}

static int staged(struct image_check *t, const char *file, const char *manifest,
                  char *const region[])
{
    struct net_run r = {0};
    char *args[] = {(char *)manifest, (char *)file, region[0], region[1],
                    region[2],        region[3],    NULL};
    int ok = native(t, "stageimage", args, &r) && net_exited0(&r);
    net_run_free(&r);
    return ok;
}

struct refusal {
    const char *id, *manifest, *diagnostic;
    int tree;
};
static const struct refusal refusals[] = {
    {"doubled slash", "dir 0755 //etc\n", "malformed directory path '//etc'", 0},
    {"synthetic directory", "dir 0755 /proc/x\n", "'/proc/x' is under a kernel-synthetic namespace",
     0},
    {"synthetic file", "text 0644 /dev/console x\n",
     "'/dev/console' is under a kernel-synthetic namespace", 0},
    {"duplicate path", "text 0644 /a x\ntext 0644 /a y\n", "'/a' is staged more than once", 0},
    {"duplicate tree path", "text 0644 /a x\ntext 0644 /a y\n", "'/a' is staged more than once", 1},
    {"parent is file", "text 0644 /a x\ndir 0755 /a/b\n",
     "path '/a' descends through non-directory 'a'", 0},
    {"directory ID conflict", "dir 0755 /bC\ndir 0755 /cb\n",
     "directory '/cb' has the id of directory '/bC'", 0},
    {"implicit directory ID conflict", "dir 0755 /bC\ntext 0644 /cb/x y\n",
     "directory '/cb' has the id of directory '/bC'", 0}};

static int manifest_file(const char *file, const char *text)
{
    FILE *f = fopen(file, "wb");
    int ok;
    if (f == NULL)
        return 0;
    ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static void refusal_rows(struct image_check *t, const char *file, const char *manifest)
{
    char *empty[] = {NULL, NULL, NULL, NULL};
    unsigned i;
    for (i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        struct net_run r = {0};
        char directory[4096] = "";
        char *args[] = {(char *)manifest, (char *)file, NULL, NULL};
        int ok = manifest_file(manifest, refusals[i].manifest);
        if (refusals[i].tree) {
            ok = ok && path(t, "tree-refusal", directory);
            args[1] = "--tree";
            args[2] = directory;
        } else
            ok = ok && file_size(file, 65536) && format(t, file, empty);
        ok = ok && native(t, "stageimage", args, &r) && r.end.end == ETUDE_END_EXITED &&
             r.end.value != 0 &&
             ((r.out && strstr(r.out, refusals[i].diagnostic)) ||
              (r.err && strstr(r.err, refusals[i].diagnostic)));
        image_row(t, refusals[i].id, ok);
        net_run_free(&r);
        if (refusals[i].tree && directory[0] && !image_remove_tree(directory))
            t->failed++;
    }
}

static void full_row(struct image_check *t, const char *file, const char *manifest)
{
    char large[4096], text[4400];
    char *empty[] = {NULL, NULL, NULL, NULL};
    struct net_run r = {0};
    char *args[] = {(char *)manifest, (char *)file, NULL};
    int ok = path(t, "large", large) && file_size(large, 128 * 1024);
    snprintf(text, sizeof(text), "file 0644 /big evidence/krffs-large\n");
    ok = ok && manifest_file(manifest, text) && file_size(file, 65536) && format(t, file, empty) &&
         native(t, "stageimage", args, &r) && r.end.end == ETUDE_END_EXITED && r.end.value != 0 &&
         ((r.out && strstr(r.out, "not enough space for 'big'")) ||
          (r.err && strstr(r.err, "not enough space for 'big'")));
    image_row(t, "stager refuses a file larger than the image", ok);
    net_run_free(&r);
}

static void ribbon(struct image_check *t, const char *file)
{
    struct net_run r = {0};
    const char *p, *end;
    char bars[512] = "";
    char *args[] = {(char *)file, "-t", NULL};
    int ok = native(t, "fsviz", args, &r) && net_exited0(&r);
    p = r.out;
    while (p && (p = strchr(p, '[')) != NULL) {
        char bar[128];
        size_t at = 0;
        if (strchr("*#-", p[1]) == NULL) {
            p++;
            continue;
        }
        end = p + 2;
        while (*end == ' ' || (*end >= 'a' && *end <= 'z'))
            end++;
        while (p < end && at + 1 < sizeof(bar)) {
            if (*p != ' ' || at == 0 || bar[at - 1] != ' ')
                bar[at++] = *p;
            p++;
        }
        bar[at] = '\0';
        if (strlen(bars) + at + 2 >= sizeof(bars)) {
            ok = 0;
            break;
        }
        strcat(bars, bar);
        strcat(bars, "|");
    }
    image_row(t, "visualiser ribbon order",
              ok && strcmp(bars, "[* root node|[# alpha|[# beta|[# gamma|[- free node|") == 0);
    net_run_free(&r);
}

static void compact(struct image_check *t, const char *file, const char *manifest, int region)
{
    char position[32];
    char *empty[] = {NULL, NULL, NULL, NULL};
    char *slice[] = {"--offset", "1048576", "--size", "65536"};
    char **options = region ? slice : empty;
    struct net_run r = {0};
    struct ring before, after;
    char *edit[] = {(char *)file, position,   "--set-type", "free", options[0],
                    options[1],   options[2], options[3],   NULL};
    char *defrag[] = {(char *)file, options[0], options[1], options[2], options[3], NULL};
    int ok = file_size(file, region ? 1152 * 1024 : 65536) && format(t, file, options) &&
             staged(t, file, manifest, options);
    before = read_ring(file, region ? 1048576 : 0, 65536);
    snprintf(position, sizeof(position), "%llu", (unsigned long long)before.beta);
    ok = ok && before.valid && before.beta != 0 && native(t, "edfs", edit, &r) && net_exited0(&r);
    net_run_free(&r);
    before = read_ring(file, region ? 1048576 : 0, 65536);
    ok = ok && native(t, "defrag", defrag, &r) && net_exited0(&r);
    net_run_free(&r);
    after = read_ring(file, region ? 1048576 : 0, 65536);
    image_row(t, region ? "defragment a region at 1 MiB" : "defragment a whole image",
              ok && before.valid && after.valid &&
                  strcmp(before.names, "root alpha free gamma free") == 0 &&
                  strcmp(after.names, "root alpha gamma free") == 0);
}

static void magic(struct image_check *t, const char *file)
{
    unsigned char before[65536], after[65536];
    FILE *f = fopen(file, "rb");
    struct net_run r = {0};
    char *args[] = {(char *)file, "0", "--set-magic", "4660", NULL};
    int ok = f != NULL && fread(before, 1, sizeof(before), f) == sizeof(before);
    if (f)
        fclose(f);
    ok = ok && native(t, "edfs", args, &r) && net_exited0(&r);
    net_run_free(&r);
    f = fopen(file, "rb");
    ok = ok && f != NULL && fread(after, 1, sizeof(after), f) == sizeof(after);
    if (f)
        fclose(f);
    image_row(t, "editor changes exactly the two magic bytes",
              ok && after[0] == 0x34 && after[1] == 0x12 &&
                  memcmp(before + 2, after + 2, sizeof(before) - 2) == 0);
}

/** The real build script must not publish a partially staged replacement. */
static int replacement(struct image_check *t)
{
    char command[4096], *before, *after;
    int status, ok;
    snprintf(command, sizeof(command), "mkdir -p %s/tree/krffs %s/tree/build/krffs %s/out", t->dir,
             t->dir, t->dir);
    ok = image_expect(t, command, "") &&
         image_source(t, "krffs/mkimage.sh", "tree/krffs/mkimage.sh");
    snprintf(
        command, sizeof(command),
        "cp /host-products/mkfs.krffs /host-products/stageimage.krffs %s/tree/build/krffs/; "
        "truncate -s 1048576 %s/out/image.krffs && /host-products/mkfs.krffs %s/out/image.krffs",
        t->dir, t->dir, t->dir);
    before = image_command(t, command, &status);
    free(before);
    ok = ok && status == 0 && image_text(t, "old.manifest", "text 0644 /old.txt previous\n") &&
         image_text(t, "bad.manifest",
                    "text 0644 /new.txt next\nfile 0644 /missing.bin /not-present\n");
    snprintf(command, sizeof(command),
             "/host-products/stageimage.krffs %s/old.manifest %s/out/image.krffs >/dev/null; "
             "/host-products/sha256 %s/out/image.krffs",
             t->dir, t->dir, t->dir);
    before = image_command(t, command, &status);
    ok = ok && status == 0;
    snprintf(command, sizeof(command),
             "/bin/sh %s/tree/krffs/mkimage.sh %s/bad.manifest %s/out/image.krffs 1", t->dir,
             t->dir, t->dir);
    after = image_command(t, command, &status);
    free(after);
    ok = ok && status != 0;
    snprintf(command, sizeof(command), "/host-products/sha256 %s/out/image.krffs", t->dir);
    after = image_command(t, command, &status);
    ok = ok && status == 0 && before && after && strcmp(before, after) == 0;
    free(before);
    free(after);
    snprintf(command, sizeof(command), "ls -A %s/out", t->dir);
    return ok && image_expect(t, command, "image.krffs\n");
}

int image_krffs(struct image_check *t)
{
    char file[4096], manifest[4096];
    char *empty[] = {NULL, NULL, NULL, NULL};
    const char *checker = etude_program(t->u.c, "host-krffs-fsck");
    const char *old_env = getenv("KRFFS_FSCK");
    char *old = old_env != NULL ? strdup(old_env) : NULL;
    if (!path(t, "image", file) || !path(t, "manifest", manifest) || checker == NULL ||
        (old_env != NULL && old == NULL)) {
        free(old);
        return 0;
    }
    if (setenv("KRFFS_FSCK", checker, 1) != 0) {
        free(old);
        return 0;
    }
    refusal_rows(t, file, manifest);
    full_row(t, file, manifest);
    image_row(t, "failed replacement keeps old image and removes temporaries", replacement(t));
    if (manifest_file(manifest, "text 0644 /alpha a\ntext 0644 /beta bb\ntext 0644 /gamma ccc\n") &&
        file_size(file, 65536) && format(t, file, empty) && staged(t, file, manifest, empty)) {
        ribbon(t, file);
        magic(t, file);
        compact(t, file, manifest, 0);
        compact(t, file, manifest, 1);
    } else
        t->failed++;
    if (old)
        setenv("KRFFS_FSCK", old, 1);
    else
        unsetenv("KRFFS_FSCK");
    free(old);
    return t->failed == 0;
}
