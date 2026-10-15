#!/bin/sh
# Prove rtl8139_hosttest.c with one private mutation of
# rach2/sys/network/driver/rtl8139.c: a warning for every receive overflow,
# not one a boot.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# overflow_once FILE: exit unless the check the fault replaces is once in FILE.
overflow_once()
{
    if [ "$(grep -c 'if (!rx_overflow_logged)' "$1" 2>/dev/null)" != 1 ]; then
        echo "rtl8139-mutation: the overflow check is not once in $1" >&2
        exit 1
    fi
}

if [ "${1:-}" = --texts ]; then
    overflow_once rach2/sys/network/driver/rtl8139.c
    echo 'rtl8139-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" rtl8139-mutation "$repo/tests/host/rtl8139_mutate.sh" "$@"
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-rtl8139.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
source="$work/sys/network/driver/rtl8139.c"
cp "$source" "$work/rtl8139.saved"

# cleanup: stop the running test and put the saved rtl8139.c back; the script
# calls it after the fault, and it is the exit trap.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/rtl8139.saved" "$source"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE: compile rtl8139_hosttest.c over the copy as $work/PHASE and
# run it for at most 10 seconds. The phase `mutated` must fail its row; any
# other must pass. Exit if not.
run_phase()
{
    phase=$1
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -DRACH2_HOSTTEST -g -O1 -ffreestanding \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/arch/x86" -I "$work/sys/arch/i686" -I "$work/sys/lib" \
        tests/host/rtl8139_hosttest.c \
        "$work/sys/lib/kstring.c" "$work/sys/lib/kctypes.c" \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 10; exec @ARGV or die "rtl8139-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'rtl8139-test: fail: repeated overflows produce one warning per boot' \
                "$binary.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "rtl8139-mutation: result: $phase status $status"
}

echo "rtl8139-mutation: logs: $work"
run_phase fixed
overflow_once "$source"
sed 's/if (!rx_overflow_logged)/if (1)/' "$work/rtl8139.saved" > "$source"
run_phase mutated
cleanup
run_phase restored
cmp "$work/rtl8139.saved" "$source"
