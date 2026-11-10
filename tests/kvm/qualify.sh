#!/bin/sh
#
# qualify.sh: run kvm-unit-tests' SVM tests one at a time in the host's QEMU
# on a processor profile, to see which of AMD's virtualization the outer
# layer gives a guest hypervisor.
#
# Usage: qualify.sh ACCEL PROFILE [TREE]
#
# ACCEL is QEMU's accelerator, `tcg` or `kvm`; PROFILE a processor profile
# of tools/etude/processor.c, whose -cpu value `etude check processor` gives;
# TREE the built kvm-unit-tests, by default the x86-64 KVM layer's in this
# clone, build/x86_64/vendor/gnu/kvm/opt/kvm-unit-tests. Each test of
# svm-tests.list boots alone, as kvm-unit-tests' x86/run boots it, with two
# processors and 4 GiB. A line `FILE:TEST STATUS` goes to standard output for
# each, in the format of ref/svm-*.status, STATUS being the
# test's: 0 a pass, 77 a skip, 3 a failure and 127 an unhandled exception;
# a test's console goes to TMPDIR, kept when it did not pass. QEMU is the
# program QEMU names, qemu-system-x86_64 by default. The exit status is 0.

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "usage: qualify.sh ACCEL PROFILE [TREE]" >&2
    exit 2
fi
accel=$1
tree=${3:-$REPO/build/x86_64/vendor/gnu/kvm/opt/kvm-unit-tests}
qemu=${QEMU:-qemu-system-x86_64}
make -s -C "$REPO" build/tools/etude/etude
cpu=$("$REPO/build/tools/etude/etude" check processor "$2")
[ -f "$tree/x86/svm.flat" ] || { echo "qualify: no $tree/x86/svm.flat" >&2; exit 1; }
mkdir -p "$REPO/build/tmp"
out=$(mktemp -d "${TMPDIR:-$REPO/build/tmp}/qualify.XXXXXX")
echo "qualify: $qemu -accel $accel -cpu $cpu; consoles in $out" >&2

grep -v '^#' "$DIR/svm-tests.list" | while read -r flat test; do
    set +e
    "$qemu" --no-reboot -nodefaults -device pc-testdev \
        -device isa-debug-exit,iobase=0xf4,iosize=0x4 -display none -serial stdio \
        -device pci-testdev -accel "$accel" -kernel "$tree/x86/$flat.flat" -smp 2 \
        -cpu "$cpu" -m 4g -append "$test" > "$out/$flat-$test.log" 2>&1 < /dev/null
    code=$?
    set -e
    # isa-debug-exit exits with the test's status shifted left and ORed with
    # 1: 1 for a pass, 3 for a failure and 155 for a skip, which x86/run
    # reports as 0, 3 and 77; an unhandled exception it reports as 127.
    if grep -q 'Unhandled exception' "$out/$flat-$test.log"; then
        status=127
    elif [ "$code" -eq 1 ]; then
        status=0
    elif [ "$code" -eq 155 ]; then
        status=77
    else
        status=$code
    fi
    [ "$status" -ne 0 ] || rm -f "$out/$flat-$test.log"
    echo "$flat:$test $status"
done
