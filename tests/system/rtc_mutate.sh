#!/bin/sh
# Usage: sh tests/system/rtc_mutate.sh [--host|--guest]
# The guest proof runs STATTIME on the KRFFS test root in all three phases.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mode=${1:---host}
case "$mode" in
    --host|--guest) ;;
    *) echo 'rtc-mutation: use --host or --guest' >&2; exit 2 ;;
esac
[ ! -d build/.run.lock ] || {
    echo 'rtc-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build/clock-proof
work=$(mktemp -d "$repo/build/clock-proof/mutation.XXXXXX")
cp rach2/sys/arch/x86/rtc.c "$work/source.saved"
source="$work/rtc.c"
[ "$mode" != --guest ] || source=rach2/sys/arch/x86/rtc.c
cp "$work/source.saved" "$source"
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

restore()
{
    cp "$work/source.saved" "$source"
    touch "$source"
    changed=
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        if [ "$mode" = --guest ]; then
            make -s rach2 >"$work/cleanup.build.log" 2>&1
        fi
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -g -O1 -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all -DRTC_HOST_TEST \
        -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/rtc_hosttest.c "$source" rach2/sys/arch/x86/pit.c -o "$work/$phase.host" \
        >"$work/$phase.compile.log" 2>&1
    cat "$work/$phase.compile.log"
    status=0
    "$work/$phase.host" >"$work/$phase.host.log" 2>&1 || status=$?
    cat "$work/$phase.host.log"
    [ ! -s "$work/$phase.host.log" ]
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ]
    else
        [ "$status" -gt 0 ] && [ "$status" -le 100 ]
    fi
    echo "rtc-mutation: result: $phase host status $status"
    [ "$mode" = --guest ] || return 0
    make -s rach2 >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp build/rach2/kernel "$work/$phase.kernel"
    cksum "$source" "$work/$phase.kernel" >"$work/$phase.artifacts"
    build/tests/system/runutil group sh tests/system/probe_roots.sh \
        "$work/$phase.kernel" "$work/$phase" krffs stattest \
        >"$work/$phase.guest.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.guest.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ]
        grep -F 'STATTIME OK:' "$work/$phase.stattest.serial.log" >/dev/null
    else
        [ "$status" -ne 0 ]
        grep -F 'STATTIME FAIL:' "$work/$phase.stattest.serial.log" >/dev/null
        grep -Eq 'sys_exit: init process [0-9]+ exited with status 1, returning to kernel shell' \
            "$work/$phase.stattest.serial.log"
    fi
    echo "rtc-mutation: result: $phase guest status $status"
}

echo "rtc-mutation: logs: $work"
if [ "$mode" = --guest ]; then
    make -s test-image build/tests/system/harness build/tests/system/runutil \
        >"$work/prepare.log" 2>&1
    cat "$work/prepare.log"
fi
run_phase fixed pass
changed=1
python3 - "$source" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = '*nanoseconds += phase_ns;'
if text.count(old) != 1:
    raise SystemExit('rtc-mutation: sub-tick expression is not unique')
path.write_text(text.replace(old, '*nanoseconds += phase_ns * 0u;'))
PY
run_phase coarse fail
restore
run_phase restored pass
if [ "$mode" = --guest ]; then
    if cmp -s "$work/fixed.kernel" "$work/coarse.kernel"; then
        echo 'rtc-mutation: kernel did not change' >&2; exit 1
    fi
    cmp "$work/fixed.kernel" "$work/restored.kernel"
fi
echo 'rtc-mutation: result: pass, fixed passed, coarse failed, restored passed'
