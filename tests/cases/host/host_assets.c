/* Compare the guest's staged sources with the declared prepared artifact. */
#include "host_assets.h"
#include "etude/artifact.h"
#include <stdio.h>
#include <string.h>

int host_assets_match(struct etude_case *c, struct etude_case_machine *m)
{
    const char *archive = etude_artifact(c, "scripts-assets");
    const char *args[] = {"/bin/sha256sum", "/host-products/scripts-assets.tar", NULL};
    struct etude_guest_result r;
    struct stat st;
    char digest[65], expected[160], actual[160];
    const char *path;
    FILE *f;
    size_t n;
    int ok;

    if (!archive || etude_digest_file(NULL, archive, &st, digest) != 0 ||
        etude_guest_command(m, args, NULL, NULL, "products-source-digest", &r) != ETUDE_OK ||
        !r.complete || r.termination.end != ETUDE_END_EXITED || r.termination.value != 0 ||
        r.cleanup != ETUDE_CLEANUP_CONFIRMED)
        return 0;
    path = etude_evidence(c, "products-source-digest");
    f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return 0;
    snprintf(expected, sizeof(expected), "%s  /host-products/scripts-assets.tar\n", digest);
    n = fread(actual, 1, sizeof(actual), f);
    ok = !ferror(f) && n == strlen(expected) && !memcmp(actual, expected, n);
    if (fclose(f))
        ok = 0;
    return ok;
}
