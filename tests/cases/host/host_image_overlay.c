/* host_image_overlay.c: execute mkext2.sh with two build umasks and failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"

/** Prepare only the product's tree, inputs and allowed executable. */
static int prepare(struct image_check *t)
{
    char command[4096], overlay[1024];
    snprintf(command, sizeof(command),
             "mkdir -p %s/tree/tools/image %s/tree/tools/setup %s/tree/tools/inspect "
             "%s/tree/build/krffs %s/tree/build/tools/inspect",
             t->dir, t->dir, t->dir, t->dir, t->dir);
    if (!image_expect(t, command, "") ||
        !image_source(t, "tools/image/mkext2.sh", "tree/tools/image/mkext2.sh") ||
        !image_source(t, "tools/setup/e2fsprogs_path.sh", "tree/tools/setup/e2fsprogs_path.sh") ||
        !image_text(t, "tree/Makefile", "build/tools/inspect/setidcheck:\n") ||
        !image_text(t, "tree/tools/inspect/setid.allow",
                    "manifest overlay-test\ntrusted overlay 0755:0:0 /usr/libexec/probe "
                    "build/layer/usr/libexec/probe\nsentinel /etc/motd 0644:0:0\ninit none\n") ||
        !image_text(t, "tree/overlay-test.manifest",
                    "dir 0755 /etc\ntext 0644 /etc/motd hello\\n\ndir 0755 /usr\ndir 1777 /tmp\n"))
        return 0;
    snprintf(overlay, sizeof(overlay),
             "#!/bin/sh\nset -e\ncp -a %s/tree/build/layer/. \"$1/\"\nprintf 'scratch\\n' > "
             "\"$1/tmp/scratch\"\n",
             t->dir);
    if (!image_put(t, "tree/overlay.sh", overlay, strlen(overlay), "0755"))
        return 0;
    snprintf(
        command, sizeof(command),
        "cp /host-products/stageimage.krffs %s/tree/build/krffs/ && cp /host-products/setidcheck "
        "%s/tree/build/tools/inspect/ && touch %s/outside && chmod 666 %s/outside",
        t->dir, t->dir, t->dir, t->dir);
    return image_expect(t, command, "");
}

/** Make the overlay input as a build under MASK, then run the actual builder. */
static char *volume(struct image_check *t, const char *mask, const char *name, int *ok)
{
    char command[8192], *text;
    int status;
    snprintf(
        command, sizeof(command),
        "set -e; rm -rf %s/tree/build/layer; umask %s; mkdir -p %s/tree/build/layer/usr/libexec "
        "%s/tree/build/layer/usr/share/probe; printf '#!/bin/sh\\n' > "
        "%s/tree/build/layer/usr/libexec/probe; chmod 755 %s/tree/build/layer/usr/libexec/probe; "
        "printf 'data\\n' > %s/tree/build/layer/usr/share/probe/data; umask 002; ln -s %s/outside "
        "%s/tree/build/layer/usr/share/probe/outside",
        t->dir, mask, t->dir, t->dir, t->dir, t->dir, t->dir, t->dir, t->dir);
    *ok = image_expect(t, command, "");
    snprintf(command, sizeof(command),
             "umask %s; PATH=/host-products:$PATH; export PATH; cd %s/tree; /bin/sh "
             "tools/image/mkext2.sh --overlay %s/tree/overlay.sh "
             "overlay-test.manifest %s/%s.ext2 16",
             mask, t->dir, t->dir, t->dir, name);
    text = image_command(t, command, &status);
    *ok = *ok && status == 0;
    free(text);
    snprintf(command, sizeof(command), "/host-products/setidcheck --nodes --ext2 %s/%s.ext2",
             t->dir, name);
    text = image_command(t, command, &status);
    *ok = *ok && status == 0 && text != NULL && *text != '\0';
    return text;
}

/** Compare the documented path, mode, owner and size fields, excluding inode. */
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
static void refused(struct image_check *t, const char *tool, const char *expected)
{
    char command[4096], relative[160], script[512], *out;
    int status, ok;
    snprintf(command, sizeof(command),
             "set -e; mkdir -p %s/fail-%s; ln -s /host-products/mke2fs %s/fail-%s/mke2fs; ln -s "
             "/host-products/debugfs "
             "%s/fail-%s/debugfs; ln -s /host-products/e2fsck %s/fail-%s/e2fsck; rm %s/fail-%s/%s",
             t->dir, tool, t->dir, tool, t->dir, tool, t->dir, tool, t->dir, tool, tool);
    ok = image_expect(t, command, "");
    snprintf(relative, sizeof(relative), "fail-%s/%s", tool, tool);
    snprintf(script, sizeof(script), "#!/bin/sh\necho 'injected failure in %s' >&2\nexit 1\n",
             tool);
    ok = ok && image_put(t, relative, script, strlen(script), "0755");
    snprintf(command, sizeof(command),
             "cd %s/tree; PATH=%s/fail-%s:$PATH /bin/sh tools/image/mkext2.sh "
             "overlay-test.manifest %s/fail.ext2 16 2>&1",
             t->dir, t->dir, tool, t->dir);
    out = image_command(t, command, &status);
    snprintf(script, sizeof(script), "injected failure in %s", tool);
    ok = ok && status != 0 && out && strstr(out, expected) && strstr(out, script);
    free(out);
    snprintf(command, sizeof(command), "test ! -e %s/fail.ext2", t->dir);
    image_row(t, tool, ok && image_expect(t, command, ""));
}

int image_overlay(struct image_check *t)
{
    char *tight, *loose, *a, *b;
    char command[512];
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
    snprintf(command, sizeof(command), "stat -c '%%a' %s/outside", t->dir);
    image_row(t, "symlink target outside tree keeps its mode", image_expect(t, command, "666\n"));
    refused(t, "mke2fs", "mkext2: mke2fs failed with status 1");
    refused(t, "debugfs", "mkext2: debugfs refused a command (status 1):");
    free(tight);
    free(loose);
    free(a);
    free(b);
    return t->failed == 0;
}
