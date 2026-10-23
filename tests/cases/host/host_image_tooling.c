/* host_image_tooling.c: C controls each signal row of the tooling product. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"

static int prepare(struct image_check *t)
{
    static const char *const dirs[] = {"tree/vendor/gnu",
                                       "tree/mk",
                                       "tree/krffs",
                                       "tree/src",
                                       "tree/bin",
                                       "tree/build/tools/etude",
                                       "tree/build/tools/verify",
                                       "tree/fix"};
    unsigned i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (!image_directory(t, dirs[i], 0))
            return 0;
    if (!image_source(t, "vendor/gnu/tooling_image.sh", "tree/vendor/gnu/tooling_image.sh") ||
        !image_source(t, "vendor/gnu/arch.sh", "tree/vendor/gnu/arch.sh") ||
        !image_source(t, "mk/arch.sh", "tree/mk/arch.sh") ||
        !image_text(t, "tree/src/crypt.c", "char crypt;\n") ||
        !image_text(t, "tree/vendor/gnu/tooling_job.sh", "") ||
        !image_put(t, "tree/krffs/manifest_deps.sh", "#!/bin/sh\nexit 0\n", 17, "0755"))
        return 0;
    return image_link(t, "/host-products/drive", "tree/bin/git") &&
           image_link(t, "/host-products/drive", "tree/build/tools/etude/etude") &&
           image_link(t, "/host-products/drive", "tree/build/tools/verify/sha256");
}

int image_tooling(struct image_check *t)
{
    static const char *const rows[] = {"pass", "term",  "int",   "hup",
                                       "bare", "alone", "twice", "kill"};
    static const char *const shells[] = {"/bin/sh", "/bin/bash"};
    char tree[1024], label[160], removed[1024];
    const char *remove[] = {IMAGE_FIXTURE, "remove", removed, NULL};
    unsigned shell, row;
    if (!prepare(t))
        return 0;
    for (shell = 0; shell < 2; shell++)
        for (row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
            int status;
            char *out;
            const char *argv[] = {"/host-products/drive", tree, shells[shell], rows[row], NULL};
            snprintf(removed, sizeof(removed), "%s/tree/build/vendor", t->dir);
            if (!image_expect(t, remove, ""))
                return 0;
            snprintf(removed, sizeof(removed), "%s/tree/build/images", t->dir);
            if (!image_expect(t, remove, "") || !image_directory(t, "tree/build/tooling", 1))
                return 0;
            snprintf(tree, sizeof(tree), "%s/tree", t->dir);
            out = image_command(t, argv, &status);
            free(out);
            snprintf(
                label, sizeof(label),
                "%s %s exits with its signal status and owns its stage and lock until the job ends",
                shells[shell], rows[row]);
            image_row(t, label, status == 0);
        }
    return t->failed == 0;
}
