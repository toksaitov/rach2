#!/bin/sh
# Prove timercal_hosttest.c with private mutations of the calibrations in
# rach2/sys/arch/x86/apictimer.c and pit.c. Three faults: the APIC timer read
# between one pair of reads of the reference, stopped or not; the APIC timer
# measured over one wait on the 8254; and the TSC measured over one. --texts
# checks that each line the faults change is once in the tree's sources, and
# compiles nothing.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# timercal_texts APICTIMER PIT: check that each line the faults change is
# once in its file; return 1 if not.
timercal_texts()
{
    for line in '#define SAMPLE_TRIES    16u' '#define PIT_WINDOWS 3u'; do
        if [ "$(grep -c "^$line\$" "$1" 2>/dev/null)" != 1 ]; then
            echo "timercal-mutation: not once in $1: $line" >&2
            return 1
        fi
    done
    if [ "$(grep -c '^#define TSC_WINDOWS     3u$' "$2" 2>/dev/null)" != 1 ]; then
        echo "timercal-mutation: not once in $2: #define TSC_WINDOWS     3u" >&2
        return 1
    fi
}

if [ "${1:-}" = --texts ]; then
    timercal_texts rach2/sys/arch/x86/apictimer.c rach2/sys/arch/x86/pit.c
    echo 'timercal-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" timercal-mutation "$repo/tests/host/timercal_mutate.sh" "$@"
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
stopped=
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-timercal.XXXXXX")
child=

# cleanup: stop the running test and put the saved sources back; the script
# calls it after each fault, and the exit trap calls it, then removes $work if
# the script exits 0 or a signal stops it.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/apictimer.saved" "$work/apictimer.c"
    cp "$work/pit.saved" "$work/pit.c"
    touch "$work/apictimer.c" "$work/pit.c"
}

cp rach2/sys/arch/x86/apictimer.c "$work/apictimer.saved"
cp rach2/sys/arch/x86/pit.c "$work/pit.saved"
cp "$work/apictimer.saved" "$work/apictimer.c"
cp "$work/pit.saved" "$work/pit.c"
st=0
trap 'st=$?; cleanup; { [ "$st" -ne 0 ] && [ -z "${stopped:-}" ]; } || [ -z "$work" ] || rm -rf "$work"' EXIT

# run_phase PHASE: compile timercal_hosttest.c over the copies as
# $work/PHASE and run it for at most 60 seconds. The phases `samples`,
# `windows` and `tsc` must fail at their faults' rows; any other must pass.
# Exit if not.
run_phase()
{
    phase=$1
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Wno-empty-body \
        -ffreestanding -I "$work" -I rach2/sys -I rach2/sys/arch/x86 -I rach2/sys/arch/i686 \
        -I rach2/sys/lib -I rach2/sys/proc -I tests/host/compat \
        tests/host/timercal_hosttest.c tests/host/compat/spinlock_host.c \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 60; exec @ARGV or die "timercal-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    case $phase in
        samples) row='the APIC timer against the PM timer: [0-9]* of [0-9]* stops gave a wrong result' ;;
        windows) row='the APIC timer against the 8254: [0-9]* of [0-9]* stops gave a wrong result' ;;
        tsc) row='the TSC against the 8254: [0-9]* of [0-9]* stops gave a wrong result' ;;
        *) row= ;;
    esac
    if [ -n "$row" ]; then
        [ "$status" -eq 1 ] && grep -q "$row" "$binary.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "timercal-mutation: result: $phase status $status"
}

# mutate FILE FROM TO: the copy of FILE with the line FROM made TO.
mutate()
{
    sed "s/^$2\$/$3/" "$work/$1.saved" >"$work/$1.c"
    touch "$work/$1.c"
    if cmp -s "$work/$1.saved" "$work/$1.c"; then
        echo "timercal-mutation: the fault did not change $1.c" >&2
        exit 1
    fi
}

echo "timercal-mutation: logs: $work"
timercal_texts "$work/apictimer.saved" "$work/pit.saved"
run_phase fixed
mutate apictimer '#define SAMPLE_TRIES    16u' '#define SAMPLE_TRIES    1u'
run_phase samples
cleanup
run_phase restored
mutate apictimer '#define PIT_WINDOWS 3u' '#define PIT_WINDOWS 1u'
run_phase windows
cleanup
run_phase restored-again
mutate pit '#define TSC_WINDOWS     3u' '#define TSC_WINDOWS     1u'
run_phase tsc
cleanup
run_phase restored-last
cmp "$work/apictimer.saved" "$work/apictimer.c"
cmp "$work/pit.saved" "$work/pit.c"
