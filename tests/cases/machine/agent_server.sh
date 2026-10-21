#!/bin/sh
# agent_server.sh: expose API readiness, output and exit status to etude.
# The supervisor's stop result is distinct from the server's own verdict.
trap '' TERM
python3 -B "${ETUDE_TREE:?}/tests/system/fakeapi/fakeapi.py" "$@" 2>&1
status=$?
printf '%s\n' "$status" > "$3.status"
exit "$status"
