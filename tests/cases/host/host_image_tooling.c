/* host_image_tooling.c: C controls each signal row of the tooling product. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"

static int prepare(struct image_check *t)
{
    char command[4096];
    snprintf(command, sizeof(command),
             "mkdir -p %s/tree/vendor/gnu %s/tree/mk %s/tree/krffs %s/tree/src %s/tree/bin "
             "%s/tree/build/tools/etude %s/tree/build/tools/verify %s/tree/fix",
             t->dir, t->dir, t->dir, t->dir, t->dir, t->dir, t->dir, t->dir);
    if (!image_expect(t, command, "") ||
        !image_source(t, "vendor/gnu/tooling_image.sh", "tree/vendor/gnu/tooling_image.sh") ||
        !image_source(t, "vendor/gnu/arch.sh", "tree/vendor/gnu/arch.sh") ||
        !image_source(t, "mk/arch.sh", "tree/mk/arch.sh") ||
        !image_text(t, "tree/src/crypt.c", "char crypt;\n") ||
        !image_text(t, "tree/vendor/gnu/tooling_job.sh", "") ||
        !image_put(t, "tree/krffs/manifest_deps.sh", "#!/bin/sh\nexit 0\n", 17, "0755"))
        return 0;
    snprintf(command, sizeof(command),
             "ln -s /host-products/drive %s/tree/bin/git; ln -s /host-products/drive "
             "%s/tree/build/tools/etude/etude; ln -s /host-products/drive "
             "%s/tree/build/tools/verify/sha256",
             t->dir, t->dir, t->dir);
    return image_expect(t, command, "");
}

int image_tooling(struct image_check *t)
{
    static const char *const rows[] = {"pass", "term",  "int",   "hup",
                                       "bare", "alone", "twice", "kill"};
    static const char *const shells[] = {"/bin/sh", "/bin/bash"};
    char command[4096], label[160];
    unsigned shell, row;
    if (!prepare(t))
        return 0;
    for (shell = 0; shell < 2; shell++)
        for (row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
            int status;
            char *out;
            snprintf(command, sizeof(command),
                     "rm -rf %s/tree/build/vendor %s/tree/build/tooling %s/tree/build/images; "
                     "mkdir -p %s/tree/build/tooling",
                     t->dir, t->dir, t->dir, t->dir);
            if (!image_expect(t, command, ""))
                return 0;
            snprintf(command, sizeof(command), "/host-products/drive %s/tree %s %s", t->dir,
                     shells[shell], rows[row]);
            out = image_command(t, command, &status);
            free(out);
            snprintf(
                label, sizeof(label),
                "%s %s exits with its signal status and owns its stage and lock until the job ends",
                shells[shell], rows[row]);
            image_row(t, label, status == 0);
        }
    return t->failed == 0;
}
