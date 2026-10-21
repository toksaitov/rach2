#!/bin/sh
# agent_relay.sh: forward one guest API connection to its allocated port.
exec python3 -B "${ETUDE_TREE:?}/tests/system/fakeapi/fakeapi.py" relay "$@"
