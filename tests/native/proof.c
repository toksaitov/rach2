/* proof.c: TinyCC pass, refusal, cancellation and connection-loss proofs. */
#define _POSIX_C_SOURCE 200809L
#include "collection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int remove_ended(struct nc_attempt *a, struct etude_native_result *r)
{
    struct etude_remote_conf conf;
    struct etude_remote *remote = NULL;
    enum etude_remote_status status;
    char why[256];
    if (!r->attempt.complete || r->attempt.cleanup != ETUDE_CLEANUP_CONFIRMED)
        return 0;
    memset(&conf, 0, sizeof(conf));
    conf.argv = a->spec.transport_argv;
    conf.run_id = a->spec.run;
    conf.journal = a->spec.journal;
    conf.evidence = a->spec.evidence;
    conf.stage_us = a->spec.stage_ms * 1000;
    conf.reconnects = a->spec.reconnects;
    status = etude_remote_open(&conf, &remote, why, sizeof(why));
    if (status != ETUDE_REMOTE_OK)
        return 0;
    status = etude_remote_remove(remote, r->attempt.attempt);
    r->removed = status == ETUDE_REMOTE_OK;
    return etude_remote_close(remote) == ETUDE_REMOTE_OK && r->removed;
}
int nc_proof_prepare(struct nc_attempt *a, const struct nc_config *conf, const char *directory,
                     int mode)
{
    if (mode == 3) {
        a->spec.cancel_ms = 1000;
        return 1;
    }
    if (mode == 4) {
        char *marker = nc_text(a, "%s/cut.marker", directory);
        for (int n = 6; n >= 0; n--)
            a->transport[n + 4] = a->transport[n];
        a->transport[0] = (char *)conf->self;
        a->transport[1] = "--cut-transport";
        a->transport[2] = marker;
        a->transport[3] = "--";
        return marker != NULL;
    }
    return 1;
}
int nc_proof_judge(struct nc_attempt *a, struct etude_native_result *r, int mode)
{
    int held;
    if (mode < 3)
        return r->status;
    held = r->transport == ETUDE_REMOTE_OK && r->attempt.complete &&
           r->attempt.cleanup == ETUDE_CLEANUP_CONFIRMED && r->attempt.end == ETUDE_END_STOPPED;
    if (mode == 3)
        held &= r->status == 3 && r->attempt.cancelled;
    if (mode == 4) {
        FILE *fp = fopen(a->transport[2], "r");
        char marker[32] = "";
        if (fp) {
            fgets(marker, sizeof(marker), fp);
            fclose(fp);
        }
        held &= r->status == 4 && r->attempt.reconnects > 0 && !strcmp(marker, "cut\n");
    }
    return held && remove_ended(a, r) ? 0 : 1;
}
int nc_proof(struct nc_config *conf)
{
    static const char *names[] = {"tccbuild.pass", "tccbuild.fail", "tccbuild.cancel",
                                  "tccbuild.cut"};
    static const char *modes[] = {"pass", "fail", "loop", "loop"};
    struct nc_inventory *inventory = calloc(1, sizeof(*inventory));
    int status;
    if (!inventory)
        return 2;
    inventory->ncases = 4;
    for (unsigned n = 0; n < 4; n++) {
        struct nc_case *c = &inventory->cases[n];
        c->name = (char *)names[n];
        c->argv[0] = "native/tccbuild.sh";
        c->argv[1] = (char *)modes[n];
        c->argc = 2;
        c->proof = (int)n + 1;
        if (n == 1) {
            c->fixtures[0] = "end:exit:1";
            c->nfixtures = 1;
        }
    }
    status = nc_collection(conf, inventory, 0, NULL, 0);
    free(inventory);
    return status;
}
