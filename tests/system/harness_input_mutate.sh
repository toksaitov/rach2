#!/bin/sh
# Compile the harness fixed, faulty and restored in a private directory, and
# run its selftest on each.
#
# The fault is an input pipe left blocking. The selftest's write to a machine
# that has stopped reading must then wait for a reader, and the selftest kills
# it after five seconds and fails; the fixed and restored builds pass.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/harness-input-proof.XXXXXX")
cp tests/system/runner/harness.c "$work/harness.c"
cp "$work/harness.c" "$work/harness.saved"
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/harness.saved" "$work/harness.c"
    touch "$work/harness.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -O2 -Wall -Wextra -Werror "$work/harness.c" -o "$binary" \
        >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/harness.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 30; exec @ARGV or die "harness-input-mutation: exec: $!\n"' \
        "$binary" --selftest >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "harness-input-mutation: result: $phase status $status"
}

echo "harness-input-mutation: logs: $work"
run_phase fixed 0 'a write the machine does not read ends at the deadline'
python3 - "$work/harness.c" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
old = ("    if (fcntl(child_out, F_SETFL, O_NONBLOCK) < 0 || "
       "fcntl(child_in, F_SETFL, O_NONBLOCK) < 0)\n")
new = "    if (fcntl(child_out, F_SETFL, O_NONBLOCK) < 0)\n"
if source.count(old) != 1:
    raise SystemExit("harness-input-mutation: source pattern is not unique: blocking")
path.write_text(source.replace(old, new))
PY
run_phase blocking-mutated 1 'a write the machine does not read was not ended by the deadline'
cleanup
run_phase blocking-restored 0 'a write the machine does not read ends at the deadline'
cmp "$work/harness.saved" "$work/harness.c"
