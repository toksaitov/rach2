/* host_scripts.c: execute a prepared C controller on the GNU products disk. */
#include "host_scripts.h"
#include <stdio.h>
int host_guest_check(struct etude_case *c, struct etude_case_machine *products, const char *program,
                     const char *assertion)
{
    const char *args[] = {program, assertion, "/tmp/etude-host-scripts", NULL};
    struct etude_guest_result r;
    enum etude_status status = etude_guest_command(products, args, NULL, NULL, assertion, &r);
    int held = status == ETUDE_OK && r.complete && r.termination.end == ETUDE_END_EXITED &&
               r.termination.value == 0 && r.cleanup == ETUDE_CLEANUP_CONFIRMED;
    if (!held && status == ETUDE_OK && r.complete && r.cleanup == ETUDE_CLEANUP_CONFIRMED) {
        char archive[256], evidence[200], label[200];
        snprintf(archive, sizeof(archive), "/tmp/etude-host-scripts/%s-evidence.tar", assertion);
        snprintf(evidence, sizeof(evidence), "%s-fixtures.tar", assertion);
        snprintf(label, sizeof(label), "%s-export", assertion);
        const char *pack[] = {"/bin/tar", "-cf", archive, "-C", "/tmp/etude-host-scripts",
                              assertion,  NULL};
        struct etude_guest_result exported;
        if (etude_guest_command(products, pack, NULL, NULL, label, &exported) == ETUDE_OK &&
            exported.complete && exported.termination.end == ETUDE_END_EXITED &&
            !exported.termination.value)
            etude_guest_get(products, archive, evidence);
    }
    (void)c;
    return held;
}
