/* The two Doom library ABIs run on matching declared guest kernels. */
#ifndef HOST_DOOM_H
#define HOST_DOOM_H
#include "etude/scenario.h"
int host_doom_check(struct etude_case *c, struct etude_case_machine *products);
#endif
