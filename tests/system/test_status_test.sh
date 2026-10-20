#!/bin/sh
# Check the test status reader, using the built runutil helper;
# tests/proofs/test-status.json runs the check against a faulty copy.
# Usage: sh tests/system/test_status_test.sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
[ "$#" -eq 0 ] || { echo 'test-status-test: usage: test_status_test.sh' >&2; exit 2; }
exec python3 "$repo/tests/system/test_status_test.py" "$repo/build/tests/system/runutil"
