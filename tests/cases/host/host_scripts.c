/* host_scripts.c: execute a prepared C controller on the GNU products disk. */
#include "host_scripts.h"
#include <stdio.h>
int host_guest_check(struct etude_case *c, struct etude_case_machine *products, const char *program,
                     const char *assertion)
{
    const char *args[] = {program, assertion, "/tmp/etude-host-scripts", NULL};
    struct etude_guest_result r;
    (void)c;
    return etude_guest_command(products, args, NULL, NULL, assertion, &r) == ETUDE_OK &&
           r.complete && r.termination.end == ETUDE_END_EXITED && r.termination.value == 0 &&
           r.cleanup == ETUDE_CLEANUP_CONFIRMED;
}
