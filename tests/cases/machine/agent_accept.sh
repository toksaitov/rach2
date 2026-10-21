#!/bin/sh
# agent_accept.sh: dispatch an acceptance action to the shared SSH driver.
exec sh "${ETUDE_TREE:?}/tests/system/accept/agents_accept.sh" etude "$@"
