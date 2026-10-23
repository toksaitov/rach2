#!/bin/sh
# Check the explicit affected suites on the selected local execution site.
set -eu
[ "$#" -ge 2 ] || {
    echo 'usage: tools/ci/push.sh NEW_OUT SUITE|ARCH:SUITE...' >&2
    exit 2
}
out=$1
shift
ETUDE_PURPOSE=push
export ETUDE_PURPOSE
exec sh tools/ci/native_entry.sh push "$out" "$@"
