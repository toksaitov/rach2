/* family_scenario.c: native ownership of one prepared product-family check. */
#include "etude/scenario.h"
#include "family_rows.h"
#include "family_metal.h"
#include "host_assets.h"
/* The shared library supplies the selected architecture, including none. */
extern const char *etude_case_architecture(const struct etude_case *c);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int execute(struct etude_case_machine *m, const char *const a[], const char *name)
{
    struct etude_guest_result r;
    return etude_guest_command(m, a, NULL, NULL, name, &r) == ETUDE_OK && r.complete &&
           r.termination.end == ETUDE_END_EXITED && !r.termination.value &&
           r.cleanup == ETUDE_CLEANUP_CONFIRMED;
}
static int prepare(struct etude_case *c, struct etude_case_machine *m)
{
    const char *mkdir[] = {"/bin/mkdir", "-p", "/host-work/tree", NULL};
    const char *untar[] = {"/bin/tar",        "-xf", "/host-products/scripts-assets.tar", "-C",
                           "/host-work/tree", NULL};
    const char *modes[] = {"/bin/chmod", "-R", "a+rX", "/host-work/tree", NULL};
    return host_assets_match(c, m) && execute(m, mkdir, "family-workspace") &&
           execute(m, untar, "family-assets") && execute(m, modes, "family-source-modes");
}
static int inputs(struct etude_case *c, struct etude_case_machine *m)
{
    const char *rw, *ro;
    if (strcmp(etude_case_id(c), "machine.diskpolicy.check"))
        return 1;
    rw = etude_artifact(c, "kernel");
    ro = etude_artifact(c, "metal-kernel");
    return rw && ro && etude_guest_put(m, rw, "/host-work/readwrite.kernel", "0644") == ETUDE_OK &&
           etude_guest_put(m, ro, "/host-work/readonly.kernel", "0644") == ETUDE_OK;
}
static int report(struct etude_case *c, const char *id, const char *const ids[], int held[40],
                  size_t n, const char *output)
{
    const char *path = etude_evidence(c, output);
    FILE *fp = path ? fopen(path, "rb") : NULL;
    char line[4096], expected[200], word[80], result[20], tail;
    size_t count = 0;
    unsigned total;
    int begun = 0, ended = 0, bad = 0;
    if (!fp)
        return 0;
    snprintf(expected, sizeof(expected), "@etude 1 begin %s\n", id);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "@etude 1 ", 9))
            continue;
        if (!begun) {
            bad |= strcmp(line, expected) != 0;
            begun = 1;
        } else if (sscanf(line, "@etude 1 end %u %c", &total, &tail) == 1) {
            bad |= ended || total != n || count != n;
            ended = 1;
        } else if (!ended && count < n &&
                   sscanf(line, "@etude 1 assert %79s %19s", word, result) == 2 &&
                   !strcmp(word, ids[count]) &&
                   (!strcmp(result, "pass") || !strcmp(result, "fail"))) {
            held[count++] = !strcmp(result, "pass");
        } else {
            bad = 1;
        }
    }
    bad |= ferror(fp);
    fclose(fp);
    return begun && ended && !bad;
}
/* A completed fault row, including its cleanup and complete report, is the
 * witness. Each row has its own declared step and guest command deadline. */
static int transport_row(struct etude_case *c, struct etude_case_machine *m,
                         const char *const env[], unsigned row, int *held)
{
    char index[12], step[80], output[80];
    const char *ids[] = {"transport"};
    const char *args[] = {"/host-products/host-family-check", "--transport-row", index,
                          "/host-work", NULL};
    struct etude_guest_result r;
    int result[40] = {0};
    snprintf(index, sizeof(index), "%u", row);
    snprintf(step, sizeof(step), "transport-%02u", row);
    snprintf(output, sizeof(output), "family-transport-%02u", row);
    if (etude_step(c, step) != ETUDE_OK ||
        etude_guest_command(m, args, env, NULL, output, &r) != ETUDE_OK || !r.complete ||
        r.cleanup != ETUDE_CLEANUP_CONFIRMED || r.termination.end != ETUDE_END_EXITED ||
        (r.termination.value != 0 && r.termination.value != 1) ||
        !report(c, etude_case_id(c), ids, result, 1, output) || r.termination.value != !result[0])
        return 0;
    *held &= result[0];
    etude_progress(c, "transport-rows", row);
    return 1;
}
static int transport_rows(struct etude_case *c, struct etude_case_machine *m,
                          const char *const env[], int held[40])
{
    held[0] = 1;
    for (unsigned row = 1; row <= FAMILY_TRANSPORT_ROWS; row++)
        if (!transport_row(c, m, env, row, &held[0]))
            return 0;
    return 1;
}
static int fixtures(struct etude_case_machine *m, const char *id)
{
    const char *select[] = {"/host-products/host-family-check", "--export", id, "/host-work", NULL};
    const char *tar[] = {
        "/bin/tar",       "-cf", "/host-work/fixtures.tar",       "-C", "/host-work", "--null",
        "--no-recursion", "-T",  "/host-work/family-export.list", NULL};
    return execute(m, select, "family-fixture-selection") &&
           execute(m, tar, "family-fixture-archive") &&
           etude_guest_get(m, "/host-work/fixtures.tar", "family-fixtures.tar") == ETUDE_OK;
}
static void assertions(struct etude_case *c, const char *const ids[], const int held[], size_t n,
                       int whole, int exported)
{
    for (size_t i = 0; i < n; i++) {
        if (!whole)
            etude_notrun(c, ids[i],
                         exported ? "the controller report is incomplete; see family-fixtures.tar"
                                  : "the controller report is incomplete; fixture export failed; "
                                    "see the retained products disk");
        else
            etude_assert(c, ids[i], held[i],
                         held[i]    ? NULL
                         : exported ? "the product result differs; see family-fixtures.tar"
                                    : "the product result differs; fixture export failed; "
                                      "see the retained products disk");
    }
}
int etude_scenario(struct etude_case *c)
{
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    struct etude_guest_result r;
    const char *id = etude_case_id(c), *ids[40], *rows = family_rows(id);
    const char *args[] = {"/host-products/host-family-check", id, "/host-work", NULL};
    char arch[80];
    const char *env[] = {arch, NULL};
    char names[1024], *at;
    size_t n = 0, i;
    int ready = 0, whole = 0, failed = 0, exported = 0, held[40] = {0};
    snprintf(arch, sizeof(arch), "ARCH=%s", etude_case_architecture(c));
    if (!rows || strlen(rows) >= sizeof(names))
        return 1;
    strcpy(names, rows);
    for (at = strtok(names, " "); at && n < 40; at = strtok(NULL, " "))
        ids[n++] = at;
    if (etude_step(c, "products") == ETUDE_OK &&
        etude_boot(c, "products", NULL, &m, &end) == ETUDE_OK &&
        etude_step(c, "prepare") == ETUDE_OK)
        ready = prepare(c, m) && inputs(c, m);
    if (etude_step(c, "check") == ETUDE_OK && ready) {
        if (!strcmp(id, "machine.metal.transport")) {
            whole = transport_rows(c, m, env, held);
            failed = !held[0];
        } else if (etude_guest_command(m, args, env, NULL, "family-report", &r) == ETUDE_OK &&
                   r.complete && r.cleanup == ETUDE_CLEANUP_CONFIRMED &&
                   r.termination.end == ETUDE_END_EXITED &&
                   (r.termination.value == 0 || r.termination.value == 1)) {
            whole = report(c, id, ids, held, n, "family-report");
            for (i = 0; i < n; i++)
                failed |= !held[i];
            whole &= r.termination.value == failed;
        }
    }
    if (ready && (!whole || failed))
        exported = fixtures(m, id);
    assertions(c, ids, held, n, whole, exported);
    return !whole || failed;
}
