#!/bin/sh
# Check the test status reader, using the built runutil helper.
# Usage: sh tests/system/test_status_test.sh [--mutate | --texts]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
case ${1:-} in
    --texts) exec python3 "$repo/tests/system/test_status_test.py" --texts ;;
    --mutate) exec python3 "$repo/tests/system/test_status_test.py" \
        "$repo/build/tests/system/runutil" --mutate ;;
    '') exec python3 "$repo/tests/system/test_status_test.py" \
        "$repo/build/tests/system/runutil" ;;
    *) echo 'test-status-test: usage: test_status_test.sh [--mutate | --texts]' >&2; exit 2 ;;
esac
