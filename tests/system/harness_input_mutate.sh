#!/bin/sh
# harness_input_mutate.sh [--texts]: compile the harness as it is, with each
# of five faults, and restored, in a private directory, and run its selftest
# on each build.
#
# The first fault is an input pipe left blocking. The selftest's write to a
# machine that has stopped reading must then wait for a reader, and the
# selftest kills it after five seconds and fails. The second is `drain_rest`
# reading nothing, which the selftest of output written before a timeout must
# fail. The third is `send_file` taking a read error for the end of its file,
# which the selftest of a `sendfile=` step whose file cannot be read must
# fail. The fourth is `never_appeared` that never reports a `--never` text,
# which the selftest of a step that waits for output after such a text must
# fail. The fifth is `wait_step` counting a failed step without reporting
# it, which the selftest of a failed step's report must fail. The fixed and
# restored builds pass. --texts checks that each fault is once in
# harness.c.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_blocking SOURCE: leave the input pipe blocking in SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_blocking()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
old = ("    if (fcntl(child_out, F_SETFL, O_NONBLOCK) < 0 || "
       "fcntl(child_in, F_SETFL, O_NONBLOCK) < 0)\n")
new = "    if (fcntl(child_out, F_SETFL, O_NONBLOCK) < 0)\n"
if source.count(old) != 1:
    raise SystemExit("harness-input-mutation: source pattern is not unique: blocking")
path.write_text(source.replace(old, new))
PY
}


# apply_drain SOURCE: make drain_rest read nothing in SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_drain()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
start = source.index("static void drain_rest(void)\n{")
old = "        n = read(child_out, chunk, sizeof(chunk));\n"
at = source.index(old, start)
if at > source.index("\n}\n", start):
    raise SystemExit("harness-input-mutation: source pattern is not in drain_rest")
path.write_text(source[:at] + "        n = 0;\n" + source[at + len(old):])
PY
}


# apply_unread SOURCE: make send_file ignore a read error in SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_unread()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
old = "    if (sf && ferror(sf))\n        sent = 0;\n"
if source.count(old) != 1:
    raise SystemExit("harness-input-mutation: source pattern is not unique: unread")
path.write_text(source.replace(old, ""))
PY
}

# apply_never SOURCE: make never_appeared report nothing in SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_never()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
old = "    return never_found;\n"
if source.count(old) != 1:
    raise SystemExit("harness-input-mutation: source pattern is not unique: never")
path.write_text(source.replace(old, "    return 0;\n"))
PY
}

# apply_report SOURCE: make wait_step count a failed step without
# reporting it in SOURCE; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_report()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import sys
import os
import pathlib
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = Path(sys.argv[1])
source = path.read_text()
old = "    if (!got)\n        step_fail(step);\n    else\n        stamp(\"MATCH\");\n"
if source.count(old) != 1:
    raise SystemExit("harness-input-mutation: source pattern is not unique: report")
path.write_text(source.replace(old, old.replace("step_fail(step);", "step_failures++;")))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_blocking tests/system/runner/harness.c >/dev/null
    apply_drain tests/system/runner/harness.c >/dev/null
    apply_unread tests/system/runner/harness.c >/dev/null
    apply_never tests/system/runner/harness.c >/dev/null
    apply_report tests/system/runner/harness.c >/dev/null
    echo 'harness-input-mutation: texts: every fault is once in its source'
    exit 0
fi


. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" harness-input-mutation "$repo/tests/system/harness_input_mutate.sh" "$@"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/harness-input-proof.XXXXXX")
cp tests/system/runner/harness.c "$work/harness.c"
cp "$work/harness.c" "$work/harness.saved"
child=

# Stop the selftest, and put back the saved private copy of harness.c.
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

# run_phase PHASE STATUS MESSAGE: compile the private copy and run its
# selftest, which must exit with STATUS and print MESSAGE.
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
apply_blocking "$work/harness.c"
run_phase blocking-mutated 1 'a write the machine does not read was not ended by the deadline'
cleanup
run_phase blocking-restored 0 'a write the machine does not read ends at the deadline'
cmp "$work/harness.saved" "$work/harness.c"
apply_drain "$work/harness.c"
run_phase late-mutated 1 'output written before a timeout was not read after it'
cleanup
run_phase late-restored 0 'output written before a timeout is read after it'
cmp "$work/harness.saved" "$work/harness.c"
apply_unread "$work/harness.c"
run_phase unread-mutated 1 'a sendfile= step whose file cannot be read passed'
cleanup
run_phase unread-restored 0 'a sendfile= step whose file cannot be read fails'
cmp "$work/harness.saved" "$work/harness.c"
apply_never "$work/harness.c"
run_phase never-mutated 1 'after a --never text appeared'
cleanup
run_phase never-restored 0 'a step that waits for output ends when a --never text appears'
cmp "$work/harness.saved" "$work/harness.c"
apply_report "$work/harness.c"
run_phase report-mutated 1 'a failed step was reported as'
cleanup
run_phase report-restored 0 'a failed step is named with its number and text'
cmp "$work/harness.saved" "$work/harness.c"
