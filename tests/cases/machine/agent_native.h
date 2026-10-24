/* agent_native.h: host C control of the guest agent workloads. */
#ifndef AGENT_NATIVE_H
#define AGENT_NATIVE_H
#include "netcase.h"

struct agent_native_config {
    const char *work, *row, *test_key;
    int fake;
};

int agent_native_action(struct net_case *u, struct net_boot *b,
                        const struct agent_native_config *config, const char *verb);
#endif
