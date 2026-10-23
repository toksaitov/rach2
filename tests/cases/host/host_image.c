/* host_image.c: fixture transfer and results for native product controllers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"

char *image_command(struct image_check *t, const char *command, int *status)
{
    const char *argv[] = {"/bin/sh", "-c", command, NULL};
    struct net_run r;
    char evidence[128];
    char *out;
    enum etude_status st;
    snprintf(evidence, sizeof(evidence), "%s-command-%u", t->id, ++t->commands);
    st = net_guest(&t->u, t->guest, argv, NULL, NULL, evidence, &r);
    *status = st == ETUDE_OK && r.end.end == ETUDE_END_EXITED ? r.end.value : -1;
    if (*status == -1)
        t->failed++;
    out = r.out;
    r.out = NULL;
    net_run_free(&r);
    return out != NULL ? out : strdup("");
}

int image_expect(struct image_check *t, const char *command, const char *expected)
{
    int status, ok;
    char *out = image_command(t, command, &status);
    ok = status == 0 && out != NULL && strcmp(out, expected) == 0;
    if (!ok)
        fprintf(stderr, "%s: command status %d, output: %s\n", t->id, status,
                out != NULL ? out : "(unavailable)");
    free(out);
    return ok;
}

int image_put(struct image_check *t, const char *relative, const void *bytes, size_t size,
              const char *mode)
{
    char name[128], host[4096], guest[1024];
    FILE *f;
    int ok;
    snprintf(name, sizeof(name), "%s-input-%u", t->id, ++t->inputs);
    if (net_path(&t->u, name, host, sizeof(host)) == NULL)
        return 0;
    f = fopen(host, "wb");
    if (f == NULL)
        return 0;
    ok = fwrite(bytes, 1, size, f) == size;
    if (fclose(f) != 0)
        ok = 0;
    snprintf(guest, sizeof(guest), "%s/%s", t->dir, relative);
    return ok && etude_guest_put(t->guest, host, guest, mode) == ETUDE_OK;
}

int image_text(struct image_check *t, const char *relative, const char *text)
{
    return image_put(t, relative, text, strlen(text), "0644");
}

int image_source(struct image_check *t, const char *source, const char *relative)
{
    static const struct {
        const char *source, *artifact;
    } sources[] = {{"utils/rach2-install", "host-source-installer"},
                   {"krffs/mkimage.sh", "host-source-mkimage"},
                   {"tools/image/mkext2.sh", "host-source-mkext2"},
                   {"tools/setup/e2fsprogs_path.sh", "host-source-e2fs-path"},
                   {"vendor/gnu/tooling_image.sh", "host-source-tooling-image"},
                   {"vendor/gnu/arch.sh", "host-source-gnu-arch"},
                   {"mk/arch.sh", "host-source-mk-arch"}};
    char guest[1024];
    size_t i;
    const char *prepared = NULL;
    for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
        if (strcmp(source, sources[i].source) == 0)
            prepared = etude_artifact(t->u.c, sources[i].artifact);
    snprintf(guest, sizeof(guest), "%s/%s", t->dir, relative);
    return prepared != NULL && etude_guest_put(t->guest, prepared, guest, "0755") == ETUDE_OK;
}

void image_row(struct image_check *t, const char *row, int held)
{
    fprintf(held ? stdout : stderr, "%s: %s: %s\n", t->id, held ? "pass" : "fail", row);
    if (!held)
        t->failed++;
}

int host_image_check(struct etude_case *c, struct etude_case_machine *products,
                     const char *assertion)
{
    struct image_check t;
    const char *argv[4];
    struct etude_guest_result r;
    char command[512];
    int ok;
    memset(&t, 0, sizeof(t));
    t.u.c = c;
    t.u.client = t.guest = products;
    t.id = assertion;
    if (strcmp(assertion, "mkext2_overlay") && strcmp(assertion, "krffs_tools") &&
        strcmp(assertion, "install_accounts") && strcmp(assertion, "tooling_image") &&
        strcmp(assertion, "collection_tools"))
        return 0;
    snprintf(t.dir, sizeof(t.dir), "/tmp/etude-image-%s", assertion);
    snprintf(command, sizeof(command), "mkdir -m 700 %s", t.dir);
    ok = image_expect(&t, command, "");
    if (!ok)
        goto cleanup;
    if (strcmp(assertion, "install_accounts") == 0)
        ok = image_accounts(&t);
    else if (strcmp(assertion, "krffs_tools") == 0)
        ok = image_krffs(&t);
    else if (strcmp(assertion, "mkext2_overlay") == 0)
        ok = image_overlay(&t);
    else if (strcmp(assertion, "tooling_image") == 0)
        ok = image_tooling(&t);
    else
        ok = image_collection(&t);
cleanup:
    snprintf(command, sizeof(command), "rm -rf %s && test ! -e %s", t.dir, t.dir);
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = command;
    argv[3] = NULL;
    if (etude_guest_command(products, argv, NULL, NULL, "image-check-cleanup", &r) != ETUDE_OK ||
        r.termination.end != ETUDE_END_EXITED || r.termination.value != 0 ||
        r.cleanup != ETUDE_CLEANUP_CONFIRMED || !r.complete)
        ok = 0;
    t.u.failures += t.failed + !ok;
    if (net_end(&t.u) != 0)
        ok = 0;
    return ok && t.failed == 0;
}
