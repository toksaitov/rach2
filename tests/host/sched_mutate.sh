#!/bin/sh
# Compile the scheduler's fixed, faulty and restored forms in a private tree.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/sched-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
cp "$work/sys/proc/sched.c" "$work/sched.saved"

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/sched.saved" "$work/sys/proc/sched.c"
    touch "$work/sys/proc/sched.c"
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
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        tests/host/sched_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/proc/sched.c" "$binary" >"$binary.cksum"
    perl -e 'alarm 10; exec @ARGV or die "sched-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "sched-mutation: result: $phase status $status"
}

echo "sched-mutation: logs: $work"
run_phase fixed 0 'sched-hosttest: result: pass:'
for fault in preference bound cursor catchup; do
    python3 - "$work/sys/proc/sched.c" "$fault" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
changes = {
    "preference": ("next = sched_find(me, 1);", "next = NULL;"),
    "bound": ("_wake_turn[me] = 0;", "_wake_turn[me] = 1;"),
    "cursor": ("_wake_turn[me] = 0;", "_last_sched[me] = next;\n        _wake_turn[me] = 0;"),
    "catchup": ("    while (ticks-- > 0) {\n        sched_account_one(cpu, user);\n    }",
                "    (void)ticks;\n    sched_account_one(cpu, user);"),
}
old, new = changes[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit("sched-mutation: source pattern is not unique")
path.write_text(source.replace(old, new))
PY
    case $fault in
        preference) message='woken sleeper precedes spinner' ;;
        bound) message='spinner missed fourteen dispatches' ;;
        cursor) message='ordinary turn follows preferred wake' ;;
        catchup) message='three global ticks since the processor' ;;
    esac
    run_phase "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$fault-restored" 0 'sched-hosttest: result: pass:'
    cmp "$work/sched.saved" "$work/sys/proc/sched.c"
done
