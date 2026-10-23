/* Native fixture setup and independent judges for the ext2 host tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "command_host.h"
#include "ext2_fixture.h"

static int text(const char *path, const char *bytes)
{
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes);
    return f != NULL && fclose(f) == 0 && ok;
}

int ext2_fixture_noise(const char *path, unsigned long bytes)
{
    unsigned char block[32768];
    FILE *in = fopen("/dev/urandom", "rb"), *out = in ? fopen(path, "wb") : NULL;
    size_t take;
    int ok = out != NULL;
    while (ok && bytes) {
        take = bytes < sizeof block ? (size_t)bytes : sizeof block;
        ok = fread(block, 1, take, in) == take && fwrite(block, 1, take, out) == take;
        bytes -= (unsigned long)take;
    }
    if (in && fclose(in))
        ok = 0;
    if (out && fclose(out))
        ok = 0;
    return ok;
}

static int sparse(const char *path, long size, const long offsets[], const char *values)
{
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && ftruncate(fileno(f), size) == 0;
    size_t i;
    for (i = 0; ok && values[i]; i++)
        ok = fseek(f, offsets[i], SEEK_SET) == 0 && fputc(values[i], f) != EOF;
    return f != NULL && fclose(f) == 0 && ok;
}

static const char *path(const char *root, const char *name)
{
    static char names[16][4096];
    static unsigned next;
    char *out = names[next++ % 16];
    if (snprintf(out, sizeof names[0], "%s/%s", root, name) >= (int)sizeof names[0])
        return NULL;
    return out;
}

int ext2_fixture_tree(const char *root)
{
    static const char *const dirs[] = {"tree", "tree/dir", "tree/dir/sub", "tree/many", "levels"};
    static const long tail[] = {5000000, 5000001, 5000002};
    static const long levels[] = {0, 12388, 300000, 69000000};
    size_t i;
    char name[128];
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
        if (mkdir(path(root, dirs[i]), 0755) != 0)
            return 0;
    if (!text(path(root, "tree/hello.txt"), "hello\n") ||
        !text(path(root, "tree/dir/sub/deep.txt"), "deep\n") ||
        !ext2_fixture_noise(path(root, "tree/dir/big.bin"), 200000) ||
        symlink("hello.txt", path(root, "tree/fast")) ||
        symlink(
            "dir/sub/deep.txt/../../../dir/sub/deep.txt/../../sub/deep.txt/../deep.txt/../deep.txt",
            path(root, "tree/slow")) ||
        link(path(root, "tree/hello.txt"), path(root, "tree/hello.link")) ||
        !sparse(path(root, "tree/sparse"), 5000003, tail, "end") ||
        !sparse(path(root, "levels/levels"), 70000000, levels, "ABCD"))
        return 0;
    for (i = 0; i < 300; i++) {
        snprintf(name, sizeof name, "tree/many/file%u.txt", (unsigned)i);
        if (!text(path(root, name), "x"))
            return 0;
    }
    return 1;
}

int ext2_fixture_image(const char *image, int block, int inode, const char *features,
                       unsigned long mib, const char *tree)
{
    char bs[32], is[32], size[32], log[4096];
    const char *argv[] = {host_tool_path("mke2fs"),
                          "-q",
                          "-F",
                          "-t",
                          "ext2",
                          "-b",
                          bs,
                          "-I",
                          is,
                          "-O",
                          features,
                          "-d",
                          tree,
                          image,
                          size,
                          NULL};
    const char *previous = getenv("E2FSPROGS_FAKE_TIME");
    char *saved = previous ? strdup(previous) : NULL;
    FILE *f;
    int status;
    if (previous && !saved)
        return 0;
    snprintf(bs, sizeof bs, "%d", block);
    snprintf(is, sizeof is, "%d", inode);
    snprintf(size, sizeof size, "%luM", mib);
    snprintf(log, sizeof log, "%s.mkfs.out", image);
    remove(image);
    f = fopen(image, "wb");
    if (!f || fclose(f) || setenv("E2FSPROGS_FAKE_TIME", "1", 1)) {
        free(saved);
        return 0;
    }
    status = host_command(argv, log, log, NULL, 0);
    int restored =
        saved ? setenv("E2FSPROGS_FAKE_TIME", saved, 1) : unsetenv("E2FSPROGS_FAKE_TIME");
    free(saved);
    return status == 0 && restored == 0;
}

int ext2_fixture_debugfs(const char *image, const char *request, int write, const char *output)
{
    const char *args[7];
    unsigned n = 0;
    args[n++] = host_tool_path("debugfs");
    if (write)
        args[n++] = "-w";
    args[n++] = "-R";
    args[n++] = request;
    args[n++] = image;
    args[n] = NULL;
    return host_command(args, output, NULL, NULL, 0) == 0;
}

int ext2_fixture_fsck(const char *image, const char *flags, const char *output)
{
    const char *argv[] = {host_tool_path("e2fsck"), flags, image, NULL};
    return host_command(argv, output, output, NULL, 0);
}

int ext2_fixture_findings(const char *input, const char *output)
{
    static const char *const bare[] = {"Fix? no", "Clear? no", "Allocate? no", "Salvage? no",
                                       "Connect to /lost+found? no"};
    char line[4096], previous[4096] = "";
    FILE *in = fopen(input, "r"), *out = in ? fopen(output, "w") : NULL;
    size_t n, i;
    int ok = out != NULL;
    while (ok && fgets(line, sizeof line, in)) {
        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n >= 4 && !strcmp(line + n - 4, "? no")) {
            for (i = 0; i < sizeof bare / sizeof bare[0] && strcmp(line, bare[i]); i++)
                ;
            ok = fprintf(out, "%s\n", i == sizeof bare / sizeof bare[0] ? line : previous) >= 0;
        }
        if (n)
            snprintf(previous, sizeof previous, "%s", line);
    }
    if (in) {
        if (ferror(in))
            ok = 0;
        if (fclose(in))
            ok = 0;
    }
    if (out && fclose(out))
        ok = 0;
    return ok;
}

int ext2_fixture_contains(const char *name, const char *wanted)
{
    char line[4096];
    FILE *f = fopen(name, "r");
    int found = 0;
    while (f && fgets(line, sizeof line, f))
        if (strstr(line, wanted))
            found = 1;
    if (f)
        fclose(f);
    return found;
}

void ext2_fixture_head(const char *name, unsigned lines)
{
    char line[4096];
    FILE *f = fopen(name, "r");
    while (f && lines-- && fgets(line, sizeof line, f))
        fputs(line, stdout);
    if (f)
        fclose(f);
}

int ext2_fixture_line(const char *image, const char *tool, const char *request, const char *match,
                      int after, const char *output)
{
    char full[4096], line[4096];
    const char *args[] = {host_tool_path(tool), image, NULL};
    FILE *in, *out;
    int ok = 0;
    snprintf(full, sizeof full, "%s.full", output);
    if (request ? !ext2_fixture_debugfs(image, request, 0, full)
                : host_command(args, full, NULL, NULL, 0) != 0)
        return 0;
    in = fopen(full, "r");
    out = in ? fopen(output, "w") : NULL;
    while (out && fgets(line, sizeof line, in)) {
        if (strstr(line, match)) {
            if (!after || fgets(line, sizeof line, in))
                ok = fputs(line, out) >= 0;
            break;
        }
    }
    if (in)
        fclose(in);
    if (out && fclose(out))
        ok = 0;
    return ok;
}
