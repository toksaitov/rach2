/* Invoke one C-controlled product regression in the declared GNU helper. */
#include <stdio.h>
#include "etude/scenario.h"

int host_build_check(struct etude_case *c, struct etude_case_machine *products, const char *id)
{
    struct etude_termination end;
    char invocation[128];
    (void)c;
    int n = snprintf(invocation, sizeof(invocation), "build-%s", id);
    if (n < 0 || n >= (int)sizeof(invocation))
        return 0;
    return etude_guest(products, invocation, &end) == ETUDE_OK && end.end == ETUDE_END_EXITED &&
           end.value == 0;
}
