#!/bin/sh
# Prove the readers' bounds and exported file lengths in the fixture builders
# tests/host/mkexfatfixture.c and mkfatfixture.c, through exfatcheck_hosttest.c
# and fatcheck_hosttest.c. Each mutation is written into the builder itself,
# and restore puts the saved copy back.
#
# Usage: fixture_readers_mutate.sh [MUTATION...] | --texts
# With no MUTATION it proves every one.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_fault FILE MUTATION: put MUTATION into FILE, a fixture builder; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
changes = {
    "path": [
        ("        size_t n;\n", "        size_t n;\n        char want[600];\n"),
        ("        n = slash ? (size_t)(slash - p) : strlen(p);\n",
         "        n = slash ? (size_t)(slash - p) : strlen(p);\n"
         "        memcpy(want, p, n);\n        want[n] = 0;\n"),
        ("strlen(name) == n && memcmp(name, p, n) == 0", "strcmp(name, want) == 0"),
    ],
    "extent": [("if (nofat && !read_extent(v, first, length))",
                "if (0 && nofat && !read_extent(v, first, length))")],
    "length": [("    if (valid > length || (length && nofat",
                "    length = valid;\n    if (valid > length || (length && nofat")],
    "ordinal": [("seq >= 1 && seq <= 20 &&", "seq >= 1 &&")],
    "restart": [("pending == 1 && seq >= 1", "pending >= 1 && seq >= 1")],
}
for before, after in changes[sys.argv[2]]:
    if source.count(before) != 1:
        sys.exit("fixture-mutation: source no longer matches: " + before)
    source = source.replace(before, after)
path.write_text(source)
PY
}
# select_mutation MUTATION: set `kind`, exfat or fat, for the builder MUTATION
# changes, and `expected` to the failure its check test's row prints.
select_mutation()
{
    case "$1" in
        path) kind=exfat; expected='FAIL: oversized path component' ;;
        extent) kind=exfat; expected='FAIL: reject directory after heap' ;;
        length) kind=exfat; expected='FAIL: zero tail through DataLength' ;;
        ordinal) kind=fat; expected="FAIL: 'reject twenty-one-entry name'" ;;
        restart) kind=fat; expected="FAIL: 'reject repeated start before twenty entries'" ;;
        *) echo "fixture-mutation: unknown mutation: $1" >&2; exit 1 ;;
    esac
}

mutations='path extent length ordinal restart'
if [ "${1:-}" = --texts ]; then
    for mutation in $mutations; do
        select_mutation "$mutation"
        RACH2_FAULT_TEXTS=1 apply_fault "tests/host/mk${kind}fixture.c" "$mutation" >/dev/null ||
            exit 1
    done
    echo 'fixture-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" fixture-mutation "$repo/tests/host/fixture_readers_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d build/fixture-readers-proof.XXXXXX)
child=
source=
cp -p tests/host/mkexfatfixture.c "$work/mkexfatfixture.c.saved"
cp -p tests/host/mkfatfixture.c "$work/mkfatfixture.c.saved"

# restore: put the saved copy back over the builder a mutation is in, if any,
# and check that it is the same.
restore()
{
    if [ -n "$source" ]; then
        cp -p "$work/$(basename "$source").saved" "$source"
        touch "$source"
        cmp "$work/$(basename "$source").saved" "$source"
        source=
    fi
}

# cleanup: the exit trap; stop the running test and restore the builder.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE KIND EXPECTED: compile KINDcheck_hosttest.c and run it in a
# session of its own for at most 60 seconds. With EXPECTED empty it must pass;
# otherwise it must fail with a status below 124 and print EXPECTED. Exit if
# not.
run_phase()
{
    phase=$1
    kind=$2
    expected=$3
    binary="$work/${kind}check_hosttest"
    log="$work/$phase-$kind.log"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all -I tests/host/compat \
        "-DFIXDIR=\"$work\"" "tests/host/${kind}check_hosttest.c" \
        tests/host/compat/fixdir_host.c -o "$binary" \
        > "$log.compile" 2>&1; then
        cat "$log.compile" >&2
        exit 1
    fi
    python3 - "$binary" > "$log" 2>&1 <<'PY' &
import os
import signal
import subprocess
import sys


def interrupted(signum, frame):
    raise SystemExit(128 + signum)


for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(sig, interrupted)
process = subprocess.Popen([sys.argv[1]], start_new_session=True)
try:
    try:
        status = process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        print("fixture-mutation: test exceeded 60 seconds", flush=True)
        status = 124
finally:
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, signal.SIG_IGN)
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()
sys.exit(status)
PY
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$log"
    cksum "tests/host/mk${kind}fixture.c" "$binary" "$work/mk${kind}fixture" > "$log.cksum"
    if [ -n "$expected" ]; then
        if [ "$status" -eq 0 ] || [ "$status" -ge 124 ] ||
            ! grep -Fq "$expected" "$log"; then
            echo "fixture-mutation: unexpected result: $phase status $status" >&2
            exit 1
        fi
        echo "fixture-mutation: result: $phase failed the expected row"
    else
        [ "$status" -eq 0 ] || exit 1
        echo "fixture-mutation: result: $phase $kind passed"
    fi
}

echo "fixture-mutation: logs: $work"
run_phase fixed exfat ''
run_phase fixed fat ''
if [ "$#" -eq 0 ]; then
    # shellcheck disable=SC2086 # the mutations are separate words
    set -- $mutations
fi
for mutation do
    select_mutation "$mutation"
    source="tests/host/mk${kind}fixture.c"
    apply_fault "$source" "$mutation"
    touch "$source"
    run_phase "$mutation" "$kind" "$expected"
    restore
    run_phase "restored-$mutation" "$kind" ''
done
echo "fixture-mutation: result: all sources restored and rebuilt"
