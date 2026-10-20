#!/bin/sh
# Prove etude_qemu_hosttest.c with faults in private copies of the launcher:
# a refused accelerator, a cancellation, a wrong accelerator in use, a
# boundary boot started without its whole memory reservation, an SMBIOS
# vendor whose commas are not doubled and one that is not printable ASCII
# must each be found.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# Change the unique fault text in a private source, or just locate it.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'memory': ('if (spec->memory_reserved_bytes < memory.resident_bytes + memory.overhead_bytes) {',
               'if (0) {'),
    'refused': ('if (m->r.state == ETUDE_MACHINE_STARTING && etude_qemu_accel_refused(',
                'if (0 && etude_qemu_accel_refused('),
    'cancel': ('    if (m->cancelled)\n        return ETUDE_END_STOPPED;\n', ''),
    'accel': ('if (strcmp(name, etude_accel_name(m->accel)) == 0) {', 'if (name != NULL) {'),
    'comma': ('add(&l, 1, "type=0,vendor=%s", p->smbios_vendor);',
              'add(&l, 0, "type=0,vendor=%s", p->smbios_vendor);'),
    'vendor': ('if (p->smbios_vendor != NULL && !text(p->smbios_vendor, ETUDE_SMBIOS_VENDOR_MAX))',
               'if (0 && !text(p->smbios_vendor, ETUDE_SMBIOS_VENDOR_MAX))'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-qemu-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    for fault in refused cancel accel memory; do
        apply_fault tools/etude/posix/qemu.c "$fault"
    done
    for fault in comma vendor; do
        apply_fault tools/etude/qemu_profile.c "$fault"
    done
    echo 'etude-qemu-mutation: texts: all six faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-qemu-mutation "$repo/tests/host/etude_qemu_mutate.sh" "$@"
work=
child=

# Stop a test before removing its private sources and evidence.
cleanup()
{
    status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ] || [ -n "${stopped:-}" ]; then
        [ -z "$work" ] || rm -rf "$work"
    else
        echo "etude-qemu-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-qemu-proof.XXXXXX")
cp tools/etude/posix/qemu.c "$work/qemu.c"
cp tools/etude/qemu_profile.c "$work/qemu_profile.c"

# Build and run the real host test against the private launcher under the
# sanitizers, with its fixtures in the scratch, and require `expected` as its
# status.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tests/host -I tools/etude/include \
        -I tools/etude/posix tests/host/etude_qemu_hosttest.c "$work/qemu.c" \
        tools/etude/capture.c tools/etude/history_json.c "$work/qemu_profile.c" \
        tools/etude/processor.c tools/etude/posix/exec.c tools/etude/posix/keeper.c \
        tools/etude/posix/process.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    TMPDIR=$work perl -e 'alarm 240; exec @ARGV or die "etude-qemu-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-qemu-mutation: result: $phase status $status"
}
run_phase fixed 0
apply_fault "$work/qemu.c" refused
run_phase refused-mutated 1
grep -q 'refuse: ended qemu_exited, not accel_failed' "$work/refused-mutated.log"
cp tools/etude/posix/qemu.c "$work/qemu.c"
apply_fault "$work/qemu.c" cancel
run_phase cancel-mutated 1
grep -q 'deaf: ended qemu_exited, not stopped' "$work/cancel-mutated.log"
cp tools/etude/posix/qemu.c "$work/qemu.c"
apply_fault "$work/qemu.c" accel
run_phase accel-mutated 1
grep -q 'wrong: ended qemu_exited, not accel_failed' "$work/accel-mutated.log"
cp tools/etude/posix/qemu.c "$work/qemu.c"
apply_fault "$work/qemu.c" memory
run_phase memory-mutated 1
grep -q 'unsafe boundary allocation refused before execution' "$work/memory-mutated.log"
cp tools/etude/posix/qemu.c "$work/qemu.c"
apply_fault "$work/qemu_profile.c" comma
run_phase comma-mutated 1
grep -q 'an smbios vendor: ' "$work/comma-mutated.log"
cp tools/etude/qemu_profile.c "$work/qemu_profile.c"
apply_fault "$work/qemu_profile.c" vendor
run_phase vendor-mutated 1
grep -q 'a control character in the smbios vendor: ' "$work/vendor-mutated.log"
grep -q 'an smbios vendor of 65 bytes: ' "$work/vendor-mutated.log"
cp tools/etude/qemu_profile.c "$work/qemu_profile.c"
run_phase restored 0
