/* host_image_overlay.c: run mkext2.sh with two build umasks and failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"

/** Prepare only the product's tree, inputs and allowed executable. */
static int prepare(struct image_check *t)
{
    static const char *const dirs[] = {"tree/tools/image", "tree/tools/setup", "tree/tools/inspect",
                                       "tree/build/krffs", "tree/build/tools/inspect"};
    char layer_path[1024];
    unsigned i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (!image_directory(t, dirs[i], 0))
            return 0;
    if (!image_source(t, "tools/image/mkext2.sh", "tree/tools/image/mkext2.sh") ||
        !image_source(t, "tools/setup/e2fsprogs_path.sh", "tree/tools/setup/e2fsprogs_path.sh") ||
        !image_text(t, "tree/Makefile", "build/tools/inspect/setidcheck:\n") ||
        !image_text(t, "tree/tools/inspect/setid.allow",
                    "manifest overlay-test\ntrusted overlay 0755:0:0 /usr/libexec/probe "
                    "build/layer/usr/libexec/probe\nsentinel /etc/motd 0644:0:0\ninit none\n") ||
        !image_text(t, "tree/overlay-test.manifest",
                    "dir 0755 /etc\ntext 0644 /etc/motd hello\\n\ndir 0755 /usr\ndir 1777 /tmp\n"))
        return 0;
    snprintf(layer_path, sizeof(layer_path), "%s/tree/build/layer", t->dir);
    return image_copy(t, IMAGE_FIXTURE, "tree/overlay") &&
           image_text(t, "tree/overlay.layer", layer_path) &&
           image_copy(t, "/host-products/stageimage.krffs", "tree/build/krffs/stageimage.krffs") &&
           image_copy(t, "/host-products/setidcheck", "tree/build/tools/inspect/setidcheck") &&
           image_put(t, "outside", "", 0, "0666");
}

/** Make the overlay under MASK, then run the actual builder. */
static char *volume(struct image_check *t, const char *mask, const char *name, int *ok)
{
    char cwd[1024], layer_path[1024], outside[1024], overlay[1024], disk[1024], *text;
    int status;
    const char *setup[] = {IMAGE_FIXTURE, "layer", layer_path, mask, outside, NULL};
    const char *build[] = {IMAGE_FIXTURE,
                           "run",
                           cwd,
                           mask,
                           "/host-products",
                           "separate",
                           "/bin/sh",
                           "tools/image/mkext2.sh",
                           "--overlay",
                           overlay,
                           "overlay-test.manifest",
                           disk,
                           "16",
                           NULL};
    const char *inspect[] = {"/host-products/setidcheck", "--nodes", "--ext2", disk, NULL};
    snprintf(cwd, sizeof(cwd), "%s/tree", t->dir);
    snprintf(layer_path, sizeof(layer_path), "%s/tree/build/layer", t->dir);
    snprintf(outside, sizeof(outside), "%s/outside", t->dir);
    snprintf(overlay, sizeof(overlay), "%s/tree/overlay", t->dir);
    snprintf(disk, sizeof(disk), "%s/%s.ext2", t->dir, name);
    *ok = image_expect(t, setup, "");
    text = image_command(t, build, &status);
    *ok = *ok && status == 0;
    free(text);
    text = image_command(t, inspect, &status);
    *ok = *ok && status == 0 && text != NULL && *text != '\0';
    return text;
}

/** Compare path, mode, owner and size fields, excluding inode. */
static char *nodes(const char *text)
{
    char *out = malloc(strlen(text) + 1);
    size_t used = 0;
    const char *p = text;
    if (out == NULL)
        return NULL;
    while (*p) {
        unsigned fields = 0;
        while (*p && *p != '\n' && fields < 5) {
            p += strspn(p, " \t");
            while (*p && strchr(" \t\n", *p) == NULL)
                out[used++] = *p++;
            out[used++] = ++fields == 5 ? '\n' : ' ';
        }
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    out[used] = '\0';
    return out;
}

static int mode(const char *listing, const char *path, const char *wanted)
{
    const char *p = listing;
    size_t n = strlen(path), m = strlen(wanted);
    while (p && *p) {
        if (strncmp(p, path, n) == 0 && p[n] == ' ' && strncmp(p + n + 1, wanted, m) == 0 &&
            p[n + m + 1] == ' ')
            return 1;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

/** A failing tool's diagnostic and status must survive the builder. */
static int failure_tools(struct image_check *t, const char *tool)
{
    static const char *const names[] = {"mke2fs", "debugfs", "e2fsck"};
    char directory[128], source[160], relative[256];
    unsigned i;
    snprintf(directory, sizeof(directory), "fail-%s", tool);
    if (!image_directory(t, directory, 0))
        return 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(source, sizeof(source), "/host-products/%s",
                 strcmp(names[i], tool) ? names[i] : "image-fixture");
        snprintf(relative, sizeof(relative), "%s/%s", directory, names[i]);
        if (!image_link(t, source, relative))
            return 0;
    }
    return 1;
}

static void refused(struct image_check *t, const char *tool, const char *expected)
{
    char cwd[1024], prefix[1024], disk[1024], diagnostic[160], *out;
    int status, ok = failure_tools(t, tool);
    const char *build[] = {IMAGE_FIXTURE,
                           "run",
                           cwd,
                           "002",
                           prefix,
                           "merge",
                           "/bin/sh",
                           "tools/image/mkext2.sh",
                           "overlay-test.manifest",
                           disk,
                           "16",
                           NULL};
    const char *absent[] = {IMAGE_FIXTURE, "absent", disk, NULL};
    snprintf(cwd, sizeof(cwd), "%s/tree", t->dir);
    snprintf(prefix, sizeof(prefix), "%s/fail-%s:/host-products", t->dir, tool);
    snprintf(disk, sizeof(disk), "%s/fail.ext2", t->dir);
    out = image_command(t, build, &status);
    snprintf(diagnostic, sizeof(diagnostic), "injected failure in %s", tool);
    ok = ok && status != 0 && out && strstr(out, expected) && strstr(out, diagnostic);
    free(out);
    image_row(t, tool, ok && image_expect(t, absent, ""));
}

int image_overlay(struct image_check *t)
{
    char *tight, *loose, *a, *b;
    char outside[1024];
    const char *inspect[] = {IMAGE_FIXTURE, "mode", outside, NULL};
    int tight_ok, loose_ok;
    if (!prepare(t))
        return 0;
    tight = volume(t, "022", "tight", &tight_ok);
    loose = volume(t, "002", "loose", &loose_ok);
    a = tight != NULL ? nodes(tight) : NULL;
    b = loose != NULL ? nodes(loose) : NULL;
    image_row(t, "volume under umask 022", tight_ok);
    image_row(t, "volume under umask 002", loose_ok);
    image_row(t, "same paths modes owners and sizes", a && b && strcmp(a, b) == 0 && a[0]);
    image_row(t, "overlay write bits and manifest sticky mode",
              b && mode(b, "/usr/libexec", "40755") && mode(b, "/usr/libexec/probe", "100755") &&
                  mode(b, "/usr/share/probe/data", "100644") && mode(b, "/tmp/scratch", "100644") &&
                  mode(b, "/tmp", "41777"));
    snprintf(outside, sizeof(outside), "%s/outside", t->dir);
    image_row(t, "symlink target outside tree keeps its mode", image_expect(t, inspect, "666\n"));
    refused(t, "mke2fs", "mkext2: mke2fs failed with status 1");
    refused(t, "debugfs", "mkext2: debugfs refused a command (status 1):");
    free(tight);
    free(loose);
    free(a);
    free(b);
    return t->failed == 0;
}
