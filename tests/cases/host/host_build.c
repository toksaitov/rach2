/* Invoke one C-controlled product regression in the declared GNU helper. */
#include "host_scripts.h"

int host_build_check(struct etude_case *c, struct etude_case_machine *products, const char *id)
{
    return host_guest_check(c, products, "/host-products/host-build-check", id);
}
