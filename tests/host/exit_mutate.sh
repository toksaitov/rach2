#!/bin/sh
# Compile each phase from a private copy; no running kernel image is replaced.
# Two faults: death published before the cleanup, and an exit that keeps its
# address space for the reap. --texts checks that each line the faults find
# is once in the tree's exit.c, and compiles nothing.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# exit_texts FILE: each line the two faults find is once in FILE.
exit_texts()
{
    for line in 'self->state = PROC_ZOMBIE;' 'self->in_syscall = 1;' 'mm_drop(self);' \
        'switch_page_directory(_kernel_dir);'; do
        if [ "$(grep -c "$line" "$1" 2>/dev/null)" != 1 ]; then
            echo "exit-mutation: not once in $1: $line" >&2
            return 1
        fi
    done
}

if [ "${1:-}" = --texts ]; then
    exit_texts rach2/sys/proc/exit.c
    echo 'exit-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" exit-mutation "$repo/tests/host/exit_mutate.sh" "$@"
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
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" -I rach2/sys -I rach2/sys/arch/x86 -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc \
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
    case $phase in
        mutated)
            [ "$status" -eq 1 ] &&
                grep -q 'exit-test: fail: cleanup ends as a zombie' "$binary.log" &&
                grep -q 'exit-test: fail: cleanup is not yet reapable' "$binary.log" || exit 1
            ;;
        kept)
            [ "$status" -eq 1 ] &&
                grep -q "exit-test: fail: the address space goes once, at the thread's end" \
                    "$binary.log" || exit 1
            ;;
        *)
            [ "$status" -eq 0 ] || exit 1
            ;;
    esac
    echo "exit-mutation: result: $phase status $status"
}

echo "exit-mutation: logs: $work"
run_phase fixed
exit_texts "$work/exit.saved"
sed '/self->state = PROC_ZOMBIE;/d; /self->in_syscall = 1;/a\
    self->state = PROC_ZOMBIE;
' "$work/exit.saved" > "$work/proc/exit.c"
touch "$work/proc/exit.c"
run_phase mutated
cleanup
run_phase restored
cmp "$work/exit.saved" "$work/proc/exit.c"
sed '/mm_drop(self);/d; /switch_page_directory(_kernel_dir);/d' "$work/exit.saved" \
    > "$work/proc/exit.c"
touch "$work/proc/exit.c"
run_phase kept
cleanup
run_phase restored-again
cmp "$work/exit.saved" "$work/proc/exit.c"
