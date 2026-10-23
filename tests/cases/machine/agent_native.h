/* agent_native.h: host C control of the guest agent workloads. */
#ifndef AGENT_NATIVE_H
#define AGENT_NATIVE_H
#include "netcase.h"
int agent_native_action(struct net_case *u, struct net_boot *b, const char *work, const char *row,
                        const char *verb, int fake);
#endif
