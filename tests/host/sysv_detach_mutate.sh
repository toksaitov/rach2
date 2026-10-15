#!/bin/sh
# Prove the detach error row by skipping a failed unmap in a private sysv.c.
# Each run compiles the real search and reaper under ASan and UBSan.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# Change the error return to a continued search; --texts checks its location.
apply_fault()
{
    python3 - "$1" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
old = '''            if (rc != 0) {
                return rc;
            }
            s->dtime = sysv_now();
'''
new = old.replace('return rc;', 'continue;')
if source.count(old) != 1:
    raise SystemExit('sysv-detach-mutation: fault text is not unique')
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_fault rach2/sys/ipc/sysv.c
    echo 'sysv-detach-mutation: texts: fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" sysv-detach-mutation "$repo/tests/host/sysv_detach_mutate.sh" "$@"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/sysv-detach-proof.XXXXXX")
cp -R rach2/sys "$work/sys"
cp "$work/sys/ipc/sysv.c" "$work/saved.c"
child=

# Stop the test before restoring the private source, including on a signal.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/saved.c" "$work/sys/ipc/sysv.c"
    touch "$work/sys/ipc/sysv.c"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Compile PHASE, run it with a bound and require STATUS and its result text.
run_phase()
{
    phase=$1
    expected=$2
    message=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/sysv_detach_hosttest.c \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 10; exec @ARGV or die "exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "sysv-detach-mutation: result: $phase status $status"
}

echo "sysv-detach-mutation: logs: $work"
run_phase fixed 0 'sysv-detach-hosttest: result: pass:'
apply_fault "$work/sys/ipc/sysv.c"
run_phase mutated 1 'unmap ENOMEM is returned without searching later slots'
cleanup
run_phase restored 0 'sysv-detach-hosttest: result: pass:'
cmp "$work/saved.c" "$work/sys/ipc/sysv.c"
