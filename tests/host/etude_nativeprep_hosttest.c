/*
 * etude_nativeprep_hosttest.c: the private KRFFS roots that `etude prepare
 * native` makes (tools/etude/native_prepare_image.c). This program is also
 * the KRFFS tools the preparation runs, mkfs.krffs and stageimage.krffs,
 * when it is started under their names.
 *
 * host-deps: tools/etude/native_prepare.c tools/etude/native_prepare_image.c
 * host-deps: tools/etude/native_prepare_archive.c tools/etude/json.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/capture.c
 * host-deps: utils/sha256.c tests/host/compat/fixdir_host.c
 * host-deps: krffs/krffs_allocator.c krffs/krffs_node.c
 * host-deps: krffs/krffs_file_system.c krffs/krffs_utilities.c
 * host-deps: krffs/krffs_platform.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/posix
 * host-cflags: -D_FILE_OFFSET_BITS=64
 *
 * A root's free extent must start on a block for files of each length that
 * leaves it elsewhere, the nodes staged before the padding must be kept
 * unchanged, and the padding must be one file of fewer than a block of
 * zeros, owned by root with mode 0444; a root made again must have the same
 * bytes. A manifest that names the padding's path, and one whose files leave
 * less free space than the root asks for, must be refused. An output reached
 * through a symbolic link out of the build directory must be refused.
 */
#define main krffs_mkfs_main
#include "../../krffs/mkfs.krffs.c"
#undef main
#define main krffs_stage_main
#include "../../krffs/stageimage.krffs.c"
#undef main

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixdir_host.h"
#include "native_prepare.h"
#include "pathmax_host.h"

extern char **environ;

/* Where the stager copies the root as the manifest left it, before the
 * preparation pads it. */
#define SNAPSHOT "ETUDE_NATIVEPREP_SNAPSHOT"

static int failures;

#define CHECK(cond, what)                                                 \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "etude-nativeprep-test: failed: %s\n", what); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* One node of a root, the free extent aside. */
struct node {
    uint64_t offset, size, data, parent;
    char name[257];
    unsigned mode, uid, gid;
    unsigned char times[48];
    unsigned char *bytes;
};

static uint64_t le(const unsigned char *b, size_t n)
{
    uint64_t v = 0;
    while (n--)
        v = v << 8 | b[n];
    return v;
}

static void nodes_free(struct node *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i].bytes);
    free(v);
}

/* Read every node of the root `path` but the free one. */
static struct node *nodes(const char *path, size_t *count)
{
    unsigned char h[NP_KRFFS_HEADER];
    struct stat st;
    struct node *v = NULL;
    uint64_t offset = 0;
    FILE *f = fopen(path, "rb");

    *count = 0;
    if (f == NULL || fstat(fileno(f), &st) != 0) {
        if (f)
            fclose(f);
        return NULL;
    }
    while (offset < (uint64_t)st.st_size) {
        struct node k;
        memset(&k, 0, sizeof(k));
        if (fseeko(f, (off_t)offset, SEEK_SET) || fread(h, 1, sizeof(h), f) != sizeof(h))
            break;
        k.offset = offset;
        k.size = le(h + 325, 8);
        k.data = le(h + 341, 8);
        k.parent = le(h + 349, 8);
        if (k.size < NP_KRFFS_HEADER || k.size > (uint64_t)st.st_size - offset)
            break;
        if (h[2] != 1) {
            memcpy(k.name, h + 11, 256);
            k.mode = (unsigned)le(h + 267, 2);
            k.uid = (unsigned)le(h + 269, 4);
            k.gid = (unsigned)le(h + 273, 4);
            memcpy(k.times, h + 277, sizeof(k.times));
            if (k.data > k.size - NP_KRFFS_HEADER || !(k.bytes = malloc(k.data + 1)) ||
                fread(k.bytes, 1, k.data, f) != k.data) {
                free(k.bytes);
                break;
            }
            struct node *grown = realloc(v, (*count + 1) * sizeof(*grown));
            if (!grown) {
                free(k.bytes);
                break;
            }
            v = grown;
            v[(*count)++] = k;
        }
        offset += k.size;
    }
    fclose(f);
    if (offset != (uint64_t)st.st_size) {
        nodes_free(v, *count);
        return NULL;
    }
    return v;
}

static int same_node(const struct node *a, const struct node *b)
{
    return a->offset == b->offset && a->size == b->size && a->data == b->data &&
           a->parent == b->parent && !strcmp(a->name, b->name) && a->mode == b->mode &&
           a->uid == b->uid && a->gid == b->gid && !memcmp(a->times, b->times, sizeof(a->times)) &&
           !memcmp(a->bytes, b->bytes, a->data);
}

static void write_bytes(const char *path, size_t n)
{
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; f && i < n; i++)
        fputc(n > 4096 ? 0 : 'x', f);
    if (f == NULL || fclose(f)) {
        fprintf(stderr, "etude-nativeprep-test: %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

/* Make a root from `text` and give the diagnostics it wrote. */
static int root(struct np_context *c, const char *image, const char *text, char *said, size_t size)
{
    const char *errors = fix_path("root.err");
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    FILE *log = fopen(errors, "w+");
    if (saved < 0 || log == NULL || dup2(fileno(log), STDERR_FILENO) < 0) {
        fprintf(stderr, "etude-nativeprep-test: cannot capture diagnostics\n");
        exit(2);
    }
    int status = np_root_image(c, image, "krffs", 1, text);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    size_t n = 0;
    if (fseek(log, 0, SEEK_SET) == 0)
        n = fread(said, 1, size - 1, log);
    said[n] = 0;
    fclose(log);
    if (n)
        fputs(said, stderr);
    return status;
}

/* The padding node is a file of zeros under a block, owned by root. */
static void check_padding(const struct node *pad)
{
    int zeros = 1;
    for (uint64_t b = 0; b < pad->data; b++)
        zeros = zeros && pad->bytes[b] == 0;
    CHECK(!strcmp(pad->name, &NP_ROOT_PADDING[1]) && pad->mode == 0444 && pad->uid == 0 &&
              pad->gid == 0 && zeros && pad->data < 4096,
          "the padding is a file of zeros under a block, owned by root, mode 0444");
}

/*
 * For the 1318-byte payload, the free extent of the unpadded root, `old_free`,
 * starts where a new node's times would cross a block, and that of the padded
 * root, `used`, does not.
 */
static void check_times(uint64_t old_free, uint64_t used)
{
    CHECK(old_free % 4096 == 2389, "the unpadded root leaves its free extent at 2389");
    for (uint64_t file_bytes = 0; file_bytes < 2; file_bytes++) {
        uint64_t old_time = old_free + 4 * NP_KRFFS_HEADER + file_bytes + 277;
        uint64_t new_time = used + 4 * NP_KRFFS_HEADER + file_bytes + 277;
        CHECK(old_time / 4096 != (old_time + 15) / 4096 &&
                  new_time / 4096 == (new_time + 15) / 4096,
              "a new node's times cross a block only without the padding");
    }
}

static void check_alignment(struct np_context *c, const char *image, const char *payload)
{
    static const size_t lengths[] = {0, 1, 1318, 2668, 3025};
    char text[8192], said[4096];

    fix_format(text, sizeof(text),
               "dir 0750:51000:52000 /owned\nfile 04755:51000:52000 /program %s\n", payload);
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        uint64_t used = 1, free_bytes = 0;
        size_t nbefore = 0, nafter = 0;
        char first[65] = "", again[65] = "";
        write_bytes(payload, lengths[i]);
        CHECK(root(c, image, text, said, sizeof(said)) == 0, "a root is made");
        CHECK(np_krffs_free_tail(image, 0, &used, &free_bytes) == 0 && used % 4096 == 0 &&
                  free_bytes >= 1048576,
              "the free extent starts on a block and is as large as asked");
        struct node *before = nodes(fix_path("snapshot.img"), &nbefore);
        struct node *after = nodes(image, &nafter);
        CHECK(before && after && nafter == nbefore + 1, "the padding is one node more");
        for (size_t k = 0; before && after && k < nbefore && k < nafter; k++)
            CHECK(same_node(&before[k], &after[k]), "the staged nodes are kept");
        if (after && nafter)
            check_padding(&after[nafter - 1]);
        if (lengths[i] == 1318 && before && nbefore)
            check_times(before[nbefore - 1].offset + before[nbefore - 1].size, used);
        nodes_free(before, nbefore);
        nodes_free(after, nafter);
        CHECK(np_file_hash(image, first) == 0 && root(c, image, text, said, sizeof(said)) == 0 &&
                  np_file_hash(image, again) == 0 && !strcmp(first, again),
              "a root made again has the same bytes");
    }
}

static void check_refusals(struct np_context *c, const char *image, const char *payload)
{
    char text[8192], said[4096];

    fix_format(text, sizeof(text), "file 0444 %s %s\n", NP_ROOT_PADDING, payload);
    CHECK(root(c, image, text, said, sizeof(said)) != 0 && strstr(said, "native root reserves"),
          "a file at the padding's path is refused");
    fix_format(text, sizeof(text), "dir 0755 %s\n", NP_ROOT_PADDING);
    CHECK(root(c, image, text, said, sizeof(said)) != 0 && strstr(said, "native root reserves"),
          "a directory at the padding's path is refused");
    write_bytes(payload, 10 * 1048576 - 3 * NP_KRFFS_HEADER);
    fix_format(text, sizeof(text),
               "dir 0750:51000:52000 /owned\nfile 04755:51000:52000 /program %s\n", payload);
    uint64_t used = 0, free_bytes = 0;
    CHECK(root(c, image, text, said, sizeof(said)) == 0 &&
              np_krffs_free_tail(image, 0, &used, &free_bytes) == 0 && free_bytes >= 1048576,
          "a root whose files outgrow the first volume is made again with the space asked");
}

/* Check that an output reached through a symbolic link that leaves the build
 * directory is refused, and one whose links stay inside it is accepted. */
static void check_output(struct np_context *c)
{
    char path[4096], out[4096];

    fix_format(path, sizeof(path), "%s/build", c->tree);
    mkdir(path, 0777);
    fix_format(path, sizeof(path), "%s/build/real", c->tree);
    mkdir(path, 0777);
    mkdir(fix_path("outside"), 0777);
    mkdir(fix_path("outside/run"), 0777);
    write_bytes(fix_path("outside/run/prepared.json"), 1);
    fix_format(path, sizeof(path), "%s/build/link", c->tree);
    CHECK(symlink(fix_path("outside"), path) == 0, "the outside link is made");
    fix_format(path, sizeof(path), "%s/build/inside", c->tree);
    CHECK(symlink("real", path) == 0, "the inside link is made");
    CHECK(np_output_path(c, "build/link/run", out) != 0,
          "an earlier output through a link out of the build directory is refused");
    CHECK(np_output_path(c, "build/link/new/run", out) != 0,
          "a new output through a link out of the build directory is refused");
    CHECK(np_output_path(c, "build/inside/run", out) == 0,
          "an output through a link inside the build directory is accepted");
    CHECK(np_output_path(c, "build/fresh/run", out) == 0,
          "an output below a new directory of the build directory is accepted");
    CHECK(np_output_path(c, "build/../outside/run", out) != 0,
          "an output with a `..` component is refused");
}

/* Run as stageimage.krffs, and keep a copy of a root its manifest made. */
static int stager(int argc, char **argv)
{
    int status = krffs_stage_main(argc, argv);
    const char *snapshot = getenv(SNAPSHOT), *manifest = argc == 3 ? strrchr(argv[1], '/') : NULL;
    if (status == 0 && snapshot && manifest && !strcmp(manifest, "/root.manifest")) {
        char bytes[65536];
        size_t n;
        FILE *in = fopen(argv[2], "rb"), *out = fopen(snapshot, "wb");
        while (in && out && (n = fread(bytes, 1, sizeof(bytes), in)))
            status = status || fwrite(bytes, 1, n, out) != n;
        if (!in || !out || ferror(in))
            status = 1;
        if (in)
            fclose(in);
        if (out && fclose(out))
            status = 1;
    }
    return status;
}

/**
 * Check the private KRFFS roots, or run as mkfs.krffs or stageimage.krffs
 * when started under either name.
 *
 * @return 0 when every check passed, 1 when one failed, 2 when the fixture
 *         could not be made.
 */
int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    char self[PATH_MAX], snapshot[PATH_MAX + 64];
    struct np_context c;
    char *env[256];
    size_t n = 0;

    if (!strcmp(name, "mkfs.krffs"))
        return krffs_mkfs_main(argc, argv);
    if (!strcmp(name, "stageimage.krffs"))
        return stager(argc, argv);
    memset(&c, 0, sizeof(c));
    if (realpath(argv[0], self) == NULL || realpath(fix_dir(), c.tree) == NULL ||
        symlink(self, fix_path("mkfs.krffs")) || symlink(self, fix_path("stageimage.krffs"))) {
        fprintf(stderr, "etude-nativeprep-test: the fixture: %s\n", strerror(errno));
        return 2;
    }
    fix_format(c.work, sizeof(c.work), "%s/work", c.tree);
    if (mkdir(c.work, 0700)) {
        fprintf(stderr, "etude-nativeprep-test: %s: %s\n", c.work, strerror(errno));
        return 2;
    }
    c.arch = "x86_64";
    c.build = "build/x86_64";
    c.tools[NP_MKFS] = (char *)fix_path("mkfs.krffs");
    c.tools[NP_STAGE] = (char *)fix_path("stageimage.krffs");
    fix_format(snapshot, sizeof(snapshot), "%s=%s/snapshot.img", SNAPSHOT, c.tree);
    for (size_t i = 0; environ[i] && n < 254; i++)
        if (strncmp(environ[i], SNAPSHOT "=", sizeof(SNAPSHOT)))
            env[n++] = environ[i];
    env[n++] = snapshot;
    env[n] = NULL;
    c.environment = env;

    check_alignment(&c, fix_path("root.img"), fix_path("program"));
    check_refusals(&c, fix_path("root.img"), fix_path("program"));
    check_output(&c);
    printf("etude-nativeprep-test: result: %d failures\n", failures);
    if (failures == 0)
        fix_passed();
    return failures != 0;
}
