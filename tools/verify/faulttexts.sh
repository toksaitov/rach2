#!/bin/sh
# faulttexts.sh: check the texts of every fault script's faults, building and
# running nothing.
#
# A fault script proves a test by putting faults into sources and requiring
# the test to fail. Each fault finds the text it changes in its source, and a
# change to that source can remove the text, which the proof would otherwise
# find only when it runs, hours later. Every fault script therefore takes
# --texts, which looks for each fault's text in the tree's source by the
# script's own rule, in most scripts that it occurs once, and exits. The fault
# scripts are the *_mutate.sh files under tests/, tools/dev/poll_kernels.sh,
# and every other script under tests/ or tools/ with a --mutate mode. One
# without a --texts mode is counted as a failure and not run, since it would
# start its proof.
#
# Usage: sh tools/verify/faulttexts.sh
set -u
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo" || exit 1
scripts=$( {
    ls tests/system/*_mutate.sh tests/system/accept/*_mutate.sh tests/host/*_mutate.sh \
        tools/dev/poll_kernels.sh
    grep -l -e '--mutate)' -e '= --mutate' -e "'--mutate'" tests/system/*.sh \
        tests/system/accept/*.sh tests/host/*.sh tools/*/*.sh
} | grep -v '^tools/verify/faulttexts\.sh$' | sort -u)
log=$(mktemp "${TMPDIR:-/tmp}/rach2-faulttexts.XXXXXX") || exit 1
trap 'rm -f "$log"' EXIT
checked=0
failed=0
for script in $scripts; do
    if ! grep -q -e '--texts' "$script"; then
        echo "faulttexts: $script: no --texts mode" >&2
        failed=$((failed + 1))
        continue
    fi
    if sh "$script" --texts >"$log" 2>&1 </dev/null; then
        checked=$((checked + 1))
    else
        echo "faulttexts: $script: a fault's text is not in its source:" >&2
        tail -n 5 "$log" >&2
        failed=$((failed + 1))
    fi
done
if [ "$failed" -ne 0 ]; then
    echo "faulttexts: result: fail: $failed of $((checked + failed)) fault scripts" >&2
    exit 1
fi
echo "faulttexts: result: pass: $checked fault scripts, each fault's text in its source"
