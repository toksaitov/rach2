#!/bin/sh
# anthropic_mutate.sh: prove the Messages script against Claude Code on Linux.
# --texts checks the mutation alone; otherwise BINARY names the pinned binary.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" = --texts ]; then
    exec python3 -B "$repo/tests/system/fakeapi/claude_test.py" --texts
fi
exec python3 -B "$repo/tests/system/fakeapi/claude_test.py" "$1" --mutate
