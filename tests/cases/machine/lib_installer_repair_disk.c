/* lib_installer_repair_disk.c: native control of the prepared disk fixture. */
#include "lib_installer_repair_disk.h"
#include "drive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int completed(enum etude_status status, const struct etude_guest_result *r)
{
    return status == ETUDE_OK && r->complete && r->cleanup == ETUDE_CLEANUP_CONFIRMED &&
           r->termination.end == ETUDE_END_EXITED;
}
static void result_text(struct etude_case *c, const char *output, char **out)
{
    char err_name[200], *text, *err;
    size_t n;
    if (!out)
        return;
    snprintf(err_name, sizeof(err_name), "%s.err", output);
    text = evidence_text(c, output);
    err = evidence_text(c, err_name);
    n = (text ? strlen(text) : 0) + (err ? strlen(err) : 0);
    *out = malloc(n + 1);
    if (*out)
        snprintf(*out, n + 1, "%s%s", text ? text : "", err ? err : "");
    free(text);
    free(err);
}
static int edit(struct etude_case_machine *m, const struct repair_disk_request *r,
                const char *output)
{
    char rec[32], boot[32];
    const char *args[8] = {"/bin/installer-repair-fixture", r->mode, "/dev/disk0", NULL};
    struct etude_guest_result result;
    enum etude_status status;
    snprintf(rec, sizeof(rec), "%llu", (unsigned long long)r->recovery);
    snprintf(boot, sizeof(boot), "%llu", (unsigned long long)r->boot);
    if (!strcmp(r->mode, "keep")) {
        args[3] = boot;
        args[4] = rec;
        args[5] = NULL;
    } else if (!strcmp(r->mode, "legacy") || !strcmp(r->mode, "slot")) {
        args[3] = rec;
        args[4] = r->name;
        args[5] = r->pubkey;
        args[6] = NULL;
    } else if (strcmp(r->mode, "root-keys"))
        return -1;
    memset(&result, 0, sizeof(result));
    status = etude_guest_command(m, args, NULL, NULL, output, &result);
    return completed(status, &result) ? result.termination.value : -1;
}
int repair_disk(struct etude_case *c, const struct repair_disk_request *request, const char *output,
                char **out)
{
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    const char *disks[] = {"image-disk", NULL};
    const char *sync_args[] = {"/bin/busybox", "sync", NULL};
    struct etude_guest_result synced;
    enum etude_status status, quit;
    char name[200], *reply = NULL;
    int rc;
    if (out)
        *out = NULL;
    if (!request || !request->mode || !request->disk ||
        etude_bind_disk(c, "image-helper", "image-disk", request->disk) != ETUDE_OK)
        return -1;
    status = etude_boot(c, "image-helper", disks, &m, &end);
    if (status != ETUDE_OK)
        return -1;
    rc = edit(m, request, output);
    snprintf(name, sizeof(name), "%s-sync", output);
    memset(&synced, 0, sizeof(synced));
    status = etude_guest_command(m, sync_args, NULL, NULL, name, &synced);
    if (!completed(status, &synced) || synced.termination.value)
        rc = -1;
    quit = etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    if (quit != ETUDE_OK || etude_wait_end(m, &end) != ETUDE_OK)
        rc = -1;
    result_text(c, output, out);
    return rc;
}
