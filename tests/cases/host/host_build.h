#ifndef HOST_BUILD_H
#define HOST_BUILD_H
#include "etude/scenario.h"
int host_build_check(struct etude_case *c, struct etude_case_machine *products,
                     const char *assertion);
#endif
