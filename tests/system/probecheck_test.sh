#!/bin/sh
# probecheck_test.sh: check tools/verify/probecheck.sh on a small tree in
# seven cases: complete assignments pass, and a program of tests/ or of
# tests/musl/ with no suite, an unknown suite, an assignment to a program
# that is gone, a helper without a reason and a program assigned twice fail.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/probecheck-test.XXXXXX")
trap 'rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
mkdir -p "$WORK/tests/musl" "$WORK/tests/system/runner"
printf 'PROGS = alpha beta\n' > "$WORK/tests/Makefile"
printf 'static const char *const PROBES[] = {\n    "alpha"};\n' > "$WORK/tests/system/runner/runtests.c"
printf 'default|-|core\nspecial|-|full\n' > "$WORK/tests/registry"
printf 'beta special\n' > "$WORK/tests/probe-suites"
printf 'musl/gamma child fixture\n' > "$WORK/tests/probe-helpers"
: > "$WORK/tests/musl/gamma.c"
# check STATUS NAME: run probecheck.sh over the tree and require STATUS.
check() {
    result=0
    PROBECHECK_ROOT="$WORK" sh "$REPO/tools/verify/probecheck.sh" > "$WORK/log" 2>&1 || result=$?
    if [ "$result" -ne "$1" ]; then
        cat "$WORK/log" >&2
        echo "probecheck-test: $2: expected $1, got $result" >&2
        exit 1
    fi
}
check 0 'complete assignments'
printf 'PROGS += missing\n' >> "$WORK/tests/Makefile"
check 1 'unassigned root program'
printf 'PROGS = alpha beta\n' > "$WORK/tests/Makefile"
: > "$WORK/tests/musl/missing.c"
check 1 'unassigned musl program'
rm "$WORK/tests/musl/missing.c"
printf 'beta absent\n' > "$WORK/tests/probe-suites"
check 1 'unknown suite'
printf 'beta special\nmissing special\n' > "$WORK/tests/probe-suites"
check 1 'stale assignment'
printf 'beta special\n' > "$WORK/tests/probe-suites"
printf 'musl/gamma\n' > "$WORK/tests/probe-helpers"
check 1 'helper without reason'
printf 'musl/gamma child fixture\nalpha repeated\n' > "$WORK/tests/probe-helpers"
check 1 'duplicate assignment'
echo "probecheck-test: passed: 7 cases"
