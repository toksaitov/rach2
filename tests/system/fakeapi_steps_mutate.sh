#!/bin/sh
# fakeapi_steps_mutate.sh [--texts]: prove the host test of the fake API
# server's steps, tests/system/fakeapi/fakeapi_test.py, against a server that
# uses up a repeated step at its first request. The fault goes into a private
# copy of the server, so nothing in the tree changes and nothing is built.
# --texts checks that the fault's text is once in fakeapi.py.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" = --texts ]; then
    exec python3 -B "$repo/tests/system/fakeapi/fakeapi_test.py" --texts
fi
exec python3 -B "$repo/tests/system/fakeapi/fakeapi_test.py" --mutate
