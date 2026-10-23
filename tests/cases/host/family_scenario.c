/* family_scenario.c: native ownership of one prepared product-family check. */
#include "etude/scenario.h"
#include "family_rows.h"
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
    const char *archive = etude_artifact(c, "scripts-assets");
    const char *mkdir[] = {"/bin/mkdir", "-p", "/host-work/tree", NULL};
    const char *untar[] = {"/bin/tar",        "-xf", "/host-work/assets.tar", "-C",
                           "/host-work/tree", NULL};
    const char *modes[] = {"/bin/chmod", "-R", "a+rX", "/host-work/tree", NULL};
    return archive && execute(m, mkdir, "family-workspace") &&
           etude_guest_put(m, archive, "/host-work/assets.tar", "0600") == ETUDE_OK &&
           execute(m, untar, "family-assets") && execute(m, modes, "family-source-modes");
}
static int report(struct etude_case *c, const char *id, const char *const ids[], int held[40],
                  size_t n)
{
    const char *path = etude_evidence(c, "family-report");
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
static void fixtures(struct etude_case *c, struct etude_case_machine *m, const char *id)
{
    const char *tar[] = {"/bin/tar", "-cf", "/host-work/fixtures.tar", "-C", "/host-work",
                         id,         NULL};
    if (execute(m, tar, "family-fixture-archive"))
        etude_guest_get(m, "/host-work/fixtures.tar", "family-fixtures.tar");
    (void)c;
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
    int ready = 0, whole = 0, failed = 0, held[40] = {0};
    snprintf(arch, sizeof(arch), "ARCH=%s", etude_case_architecture(c));
    if (!rows || strlen(rows) >= sizeof(names))
        return 1;
    strcpy(names, rows);
    for (at = strtok(names, " "); at && n < 40; at = strtok(NULL, " "))
        ids[n++] = at;
    if (etude_step(c, "products") == ETUDE_OK &&
        etude_boot(c, "products", NULL, &m, &end) == ETUDE_OK &&
        etude_step(c, "prepare") == ETUDE_OK)
        ready = prepare(c, m);
    if (etude_step(c, "check") == ETUDE_OK && ready &&
        etude_guest_command(m, args, env, NULL, "family-report", &r) == ETUDE_OK && r.complete &&
        r.cleanup == ETUDE_CLEANUP_CONFIRMED && r.termination.end == ETUDE_END_EXITED &&
        (r.termination.value == 0 || r.termination.value == 1)) {
        whole = report(c, id, ids, held, n);
        for (i = 0; i < n; i++)
            failed |= !held[i];
        whole &= r.termination.value == failed;
    }
    for (i = 0; i < n; i++) {
        if (!whole)
            etude_notrun(c, ids[i],
                         "the prepared controller did not return a whole confirmed report");
        else
            etude_assert(c, ids[i], held[i],
                         held[i] ? NULL : "the product result differs; see family-fixtures.tar");
    }
    if (ready && (!whole || failed))
        fixtures(c, m, id);
    return !whole || failed;
}
