#!/bin/sh
# Compile each phase from a private copy; no running kernel image is replaced.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-exit.XXXXXX")
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/exit.saved" "$work/proc/exit.c"
    touch "$work/proc/exit.c"
}

mkdir "$work/proc"
cp rach2/sys/proc/exit.c "$work/exit.saved"
cp "$work/exit.saved" "$work/proc/exit.c"
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/exit_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 10; exec @ARGV or die "exit-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'exit-test: fail: cleanup ends as a zombie' "$binary.log" &&
            grep -q 'exit-test: fail: cleanup is not yet reapable' "$binary.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "exit-mutation: result: $phase status $status"
}

echo "exit-mutation: logs: $work"
run_phase fixed
[ "$(grep -c 'current_proc->state = PROC_ZOMBIE;' "$work/exit.saved")" -eq 1 ]
sed '/current_proc->state = PROC_ZOMBIE;/d; /current_proc->in_syscall = 1;/a\
    current_proc->state = PROC_ZOMBIE;
' "$work/exit.saved" > "$work/proc/exit.c"
touch "$work/proc/exit.c"
run_phase mutated
cleanup
run_phase restored
cmp "$work/exit.saved" "$work/proc/exit.c"
