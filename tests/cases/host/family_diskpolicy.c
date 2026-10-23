/* family_diskpolicy.c: the real checker on both prepared kernel artifacts. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int family_diskpolicy(struct fixture *f)
{
    static const char *ids[] = {"read-write", "read-only", "read-write-refused",
                                "read-only-refused", "empty-refused"};
    char rw[4096], ro[4096];
    const char *files[5],
        *policies[] = {"read-write", "read-only", "read-only", "read-write", "read-only"};
    unsigned i;
    snprintf(rw, sizeof(rw), "%s/readwrite.kernel", f->root);
    snprintf(ro, sizeof(ro), "%s/readonly.kernel", f->root);
    files[0] = files[2] = rw;
    files[1] = files[3] = ro;
    files[4] = "/dev/null";
    if (!fx_copy(family_source(f, "tools/inspect/check_diskpolicy.sh"),
                 family_path(f, "context/tools/inspect/check_diskpolicy.sh")) ||
        !fx_copy("/host-products/binfind", family_path(f, "context/build/tools/inspect/binfind")) ||
        !fx_text(family_path(f, "context/Makefile"), "build/tools/inspect/binfind: ;\n"))
        return 0;
    for (i = 0; i < 5; i++) {
        const char *a[] = {"/bin/sh", family_path(f, "context/tools/inspect/check_diskpolicy.sh"),
                           files[i], policies[i], NULL};
        family_run(f, a, f->work, NULL, NULL);
        family_row(f, ids[i], i < 2 ? f->status == 0 : f->status != 0);
    }
    return 1;
}
