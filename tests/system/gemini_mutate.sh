#!/bin/sh
# gemini_mutate.sh: prove the Gemini script against Antigravity CLI on Linux.
# --texts checks the mutations alone; otherwise TARBALL names Google's tarball.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" = --texts ]; then
    exec python3 -B "$repo/tests/system/fakeapi/agy_test.py" --texts
fi
exec python3 -B "$repo/tests/system/fakeapi/agy_test.py" "$1" --mutate
