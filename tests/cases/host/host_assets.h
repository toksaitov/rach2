/* Verify the source archive already staged in the immutable products image. */
#ifndef HOST_ASSETS_H
#define HOST_ASSETS_H
#include "etude/scenario.h"
int host_assets_match(struct etude_case *c, struct etude_case_machine *m);
#endif
