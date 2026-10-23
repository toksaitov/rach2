/* host_scripts.h: one GNU helper shared by the scripted host assertions. */
#ifndef HOST_SCRIPTS_H
#define HOST_SCRIPTS_H
#include "etude/scenario.h"
int host_guest_check(struct etude_case *c, struct etude_case_machine *products, const char *program,
                     const char *assertion);
#endif
