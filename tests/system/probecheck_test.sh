#!/bin/sh
# probecheck_test.sh: check tools/verify/probecheck.sh on a small tree in
# eight cases: complete assignments pass, and a program of tests/ or of
# tests/musl/ with no suite, an unknown suite, an assignment to a program
# that is gone, a helper without a reason, a program assigned twice and a
# program of the default suite with no case fail.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=
trap 'rm -rf ${WORK:+"$WORK"}' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-probecheck-test.XXXXXX")
mkdir -p "$WORK/tests/musl" "$WORK/tests/system/runner" "$WORK/tests/cases"
printf 'PROGS = alpha beta\n' > "$WORK/tests/Makefile"
printf 'static const char *const PROBES[] = {\n    "alpha"};\n' > "$WORK/tests/system/runner/runtests.c"
printf 'default|-|core\nspecial|-|full\n' > "$WORK/tests/registry"
printf 'beta special\n' > "$WORK/tests/probe-suites"
printf 'musl/gamma child fixture\n' > "$WORK/tests/probe-helpers"
: > "$WORK/tests/musl/gamma.c"
printf '{"cases": [{"case_id": "guest.default.alpha"}]}\n' > "$WORK/tests/cases/guest.default.json"
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
printf 'musl/gamma child fixture\n' > "$WORK/tests/probe-helpers"
printf '{"cases": [{"case_id": "guest.default.beta"}]}\n' > "$WORK/tests/cases/guest.default.json"
check 1 'default program without a case'
if ! grep -Fqx 'probecheck: default program without a case of guest.default: alpha' "$WORK/log"; then
    cat "$WORK/log" >&2
    echo "probecheck-test: default program without a case: not named" >&2
    exit 1
fi
echo "probecheck-test: passed: 8 cases"
