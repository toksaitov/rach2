#!/bin/sh
# Prove the acceptance's credential tests against faults in a private guest
# script. --texts checks the faults' text without running the tests.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
if [ "${1:-}" = --texts ]; then
    exec python3 -B "$repo/tests/system/agents_credentials_test.py" --texts
fi
exec python3 -B "$repo/tests/system/agents_credentials_test.py" --mutate
