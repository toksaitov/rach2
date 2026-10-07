#!/bin/sh
# Recompile the production CPUID helper in the RNG probe for every phase.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# rc_once FILE: the operand the fault changes is once in FILE.
rc_once()
{
    if [ "$(grep -c '"+c"(rc)' "$1")" -ne 1 ]; then
        echo "rng-mutation: the cpuid operand is not once in $1" >&2
        exit 1
    fi
}

if [ "${1:-}" = --texts ]; then
    rc_once rach2/sys/arch/x86/kcpu.c
    echo 'rng-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" rng-mutation "$repo/tests/system/rng_mutate.sh" "$@"
# The architecture, from ARCH as mk/arch.sh reads it: its compiler, its text
# address, its kernel and its emulator.
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
kernel=${ARCH_BUILD#"$repo"/}/rach2/kernel
for artifact in "$kernel" build/tests/system/harness \
    build/krffs/mkfs.krffs build/krffs/stageimage.krffs build/krffs/readfile.krffs; do
    [ -x "$artifact" ] || { echo "rng-mutation: missing $artifact" >&2; exit 1; }
done
mkdir -p build/review-proof
work=$(mktemp -d "$repo/build/review-proof/rng.XXXXXX")
mkdir -p "$work/rach2/sys/lib" "$work/rach2/sys/arch/x86" "$work/tests" "$work/utils"
cp rach2/sys/arch/x86/kcpu.c rach2/sys/arch/x86/kcpu.h "$work/rach2/sys/arch/x86/"
cp rach2/sys/lib/kint.h "$work/rach2/sys/lib/"
cp tests/rngprobe.c tests/probe.h "$work/tests/"
cp utils/pingcall.h "$work/utils/"
source="$work/rach2/sys/arch/x86/kcpu.c"
cp "$source" "$work/saved.c"
child=
restore()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/saved.c" "$source"
    touch "$source"
}
trap restore EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    binary="$work/$phase.probe"
    # shellcheck disable=SC2086 # ARCH_LDFLAGS is empty or one flag
    "${CROSS_CC:-${CROSS}gcc}" "$MFLAG" -nostdlib -static -ffreestanding \
        -Wl,-Ttext="$ARCH_USER_TEXT" -Wl,--build-id=none $ARCH_LDFLAGS \
        -o "$binary" "$work/tests/rngprobe.c" >"$work/$phase.compile.log" 2>&1
    printf 'file 0755 /rngprobe %s\n' "$binary" > "$work/rng-proof.manifest"
    sh krffs/mkimage.sh "$work/rng-proof.manifest" "$work/$phase.krffs" 1 \
        >"$work/$phase.image.log" 2>&1
    build/krffs/readfile.krffs "$work/$phase.krffs" /rngprobe "$work/$phase.extracted"
    cmp "$binary" "$work/$phase.extracted"
    build/tests/system/harness --kernel "$kernel" \
        --initrd "$work/$phase.krffs" --append 'rach2.nonet rach2.noaudio' \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --log "$work/$phase.console.log" --timeout 30 \
        --ok 'RNGPROBE OK' --never 'RNGPROBE FAIL' \
        "expect=Type 'help'" 'send=exec /rngprobe' 'expect=RNGPROBE OK' \
        >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'RNGPROBE FAIL (cpuid subleaf zero)' "$work/$phase.console.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "rng-mutation: result: $phase status $status"
}
echo "rng-mutation: logs: $work"
run_phase fixed
rc_once "$source"
sed 's/"+c"(rc)/"=c"(rc)/' "$work/saved.c" > "$source"
run_phase mutated
restore
run_phase restored
cmp "$work/saved.c" "$source"
