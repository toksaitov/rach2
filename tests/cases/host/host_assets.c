/* Stage the helper's changing products and check the declared archive. */
#include "host_assets.h"
#include "etude/artifact.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/** Run ARGS in the guest with standard input from INPUT, or none, and say
 * whether it exited 0 with its cleanup confirmed. */
static int run(struct etude_case_machine *m, const char *const args[], const char *input,
               const char *output)
{
    struct etude_guest_result r;
    enum etude_status st = input ? etude_guest_command_file(m, args, NULL, input, output, &r)
                                 : etude_guest_command(m, args, NULL, NULL, output, &r);
    return st == ETUDE_OK && r.complete && r.termination.end == ETUDE_END_EXITED &&
           r.termination.value == 0 && r.cleanup == ETUDE_CLEANUP_CONFIRMED;
}

int host_assets_stage(struct etude_case *c, struct etude_case_machine *m)
{
    const char *archive = etude_artifact(c, "scripts-assets");
    const char *unpack[] = {"/bin/tar", "-x", "-o", "-f", "-", "-C", "/host-products", NULL};
    const char *args[] = {"/bin/sha256sum", "/host-products/scripts-assets.tar", NULL};
    struct stat st;
    char digest[65], expected[160], actual[160];
    const char *path;
    FILE *f;
    size_t n;
    int ok;

    if (!archive || etude_digest_file(NULL, archive, &st, digest) != 0 ||
        !run(m, unpack, "/dev/disk1", "products-unpack") ||
        !run(m, args, NULL, "products-source-digest"))
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
