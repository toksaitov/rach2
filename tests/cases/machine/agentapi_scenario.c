/* agentapi_scenario.c: upstream Codex and Muse on isolated GNU machines. */
#include "agent_native.h"
int agentapi_native(struct etude_case *c);
int etude_scenario(struct etude_case *c)
{
    return agentapi_native(c);
}
