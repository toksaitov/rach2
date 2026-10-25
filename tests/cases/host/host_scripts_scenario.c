/* Run one declared partition of the complete immutable host.list inventory. */
#include "host_scripts.h"
#include "host_image.h"
#include "host_build.h"
#include "host_doom.h"
#include "host_assets.h"
#include "host_script_inventory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int execute(struct etude_case_machine *m, const char *const args[], const char *label)
{
    struct etude_guest_result r;
    return etude_guest_command(m, args, NULL, NULL, label, &r) == ETUDE_OK && r.complete &&
           r.termination.end == ETUDE_END_EXITED && r.termination.value == 0 &&
           r.cleanup == ETUDE_CLEANUP_CONFIRMED;
}
static int prepare(struct etude_case *c, struct etude_case_machine *m)
{
    const char *mkdir[] = {"/bin/mkdir", "-p", "/tmp/etude-host-scripts/tree", NULL};
    const char *untar[] = {"/bin/tar",
                           "-xf",
                           "/host-products/scripts-assets.tar",
                           "-C",
                           "/tmp/etude-host-scripts/tree",
                           NULL};
    const char *readable[] = {"/bin/chmod", "-R", "a+rX", "/tmp/etude-host-scripts/tree", NULL};
    return host_assets_match(c, m) && execute(m, mkdir, "products-workspace") &&
           execute(m, untar, "products-assets") && execute(m, readable, "products-source-modes");
}
static int native(struct etude_case *c, const struct host_script_row *row)
{
    struct etude_termination end;
    char *args[] = {(char *)(row->kind == SCRIPT_PROTOCOL ? "fakeapi" : row->id), NULL};
    return etude_host(c, args, row->id, &end) == ETUDE_OK && end.end == ETUDE_END_EXITED &&
           !end.value;
}
int etude_scenario(struct etude_case *c)
{
    struct host_script_row inventory[HOST_SCRIPT_ROWS];
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    size_t n = 0, i, completed = 0;
    int part = host_script_case_part(etude_case_id(c));
    int listed, ready = 0, failed = 0;
    listed = part >= 0 && host_script_inventory(etude_artifact(c, "host-list"),
                                                etude_artifact(c, "host-cases"), inventory, &n);
    etude_step(c, "listed");
    etude_assert(c, "listed", listed,
                 "all script partitions cover host.list exactly once with matching dispatch");
    if (!listed)
        return 1;
    if (part != SCRIPT_NATIVE_PART && etude_step(c, "products") == ETUDE_OK &&
        etude_boot(c, "products", NULL, &m, &end) == ETUDE_OK &&
        etude_step(c, "prepare") == ETUDE_OK)
        ready = prepare(c, m);
    for (i = 0; i < n; i++) {
        const struct host_script_row *row = &inventory[i];
        const char *id = row->id;
        if ((int)row->part != part)
            continue;
        int held = 0;
        if (etude_step(c, !strcmp(id, "doom_libc") ? "doom-i686" : id) != ETUDE_OK) {
            etude_notrun(c, id, "the case stopped before this check");
            failed = 1;
            continue;
        }
        if (row->kind == SCRIPT_KERNEL || row->kind == SCRIPT_PROTOCOL)
            held = native(c, row);
        else if (!ready) {
            etude_notrun(c, id, "the products helper or its source assets did not start");
            failed = 1;
            continue;
        } else if (!strcmp(id, "doom_libc"))
            held = host_doom_check(c, m);
        else if (row->kind == SCRIPT_IMAGE)
            held = host_image_check(c, m, id);
        else if (row->kind == SCRIPT_BUILD)
            held = host_build_check(c, m, id);
        else
            held = host_guest_check(c, m, "/host-products/host-flow-check", id);
        etude_assert(c, id, held,
                     held ? "the prepared C controller passed every row"
                          : "the C controller failed; its complete output is evidence");
        if (!held)
            failed = 1;
        etude_progress(c, "checks", ++completed);
    }
    if (ready && !failed) {
        const char *clean[] = {"/bin/rm", "-rf", "/tmp/etude-host-scripts", NULL};
        if (!execute(m, clean, "products-cleanup"))
            failed = 1;
    }
    return failed;
}
