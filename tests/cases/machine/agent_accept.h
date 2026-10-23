/* agent_accept.h: C control of the SSH terminal acceptance. */
#ifndef AGENT_ACCEPT_H
#define AGENT_ACCEPT_H
#include "netcase.h"
int agent_accept_action(struct net_case *u, const char *work, const char *agent, const char *verb);
#endif
