#!/bin/sh
# rng_mutate.sh [--texts]: show that rngprobe's report catches two faults
# under the processor profile max, which declares RDSEED and RDRAND: a fault
# in kcpu_id, the CPUID helper of rach2/sys/arch/x86/kcpu.c that the probe
# compiles in, whose subleaf operand becomes an output; and a kernel whose
# krandom_is_trusted in rach2/sys/lib/krandom.c trusts neither instruction.
#
# The probe is built from a private copy of its sources for each phase, for
# the architecture ARCH names, and its report records are judged by
# tools/etude/casecheck as the case guest.default.rngprobe of
# tools/etude/schemas/case.guest.example.json, placed qemu. It must pass;
# then fail its assertion cpuid-leaf7 under the first fault; then, with the
# kernel's fault built into the main tree's kernel, take the fail-closed
# branch, which the probe and the old suites accept and the judge refuses
# since the profile gives the trusted one; then pass with both sources
# restored and the restored kernel the same as the first. --texts checks that
# each fault is once in its source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# once FILE TEXT: TEXT, which a fault changes, is once in FILE.
once()
{
    if [ "$(grep -cF "$2" "$1" 2>/dev/null)" != 1 ]; then
        echo "rng-mutation: the text a fault changes is not once in $1: $2" >&2
        exit 1
    fi
}
trust='return cc_src == KRANDOM_SRC_RDSEED || cc_src == KRANDOM_SRC_RDRAND;'
distrust='return 0;'

if [ "${1:-}" = --texts ]; then
    once rach2/sys/arch/x86/kcpu.c '"+c"(rc)'
    once rach2/sys/lib/krandom.c "$trust"
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
tree=${ARCH_BUILD#"$repo"/}
kernel=$tree/rach2/kernel
casecheck=build/tools/etude/casecheck
manifest=tools/etude/schemas/case.guest.example.json
krandom=rach2/sys/lib/krandom.c
mkdir -p build/review-proof
work=
child=
keep=
changed=

# Stop the running boot.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
}

# Remove krandom.c's object and the kernel, and make the kernel again.
rebuild()
{
    rm -f "$tree/rach2/sys/lib/krandom.o" "$kernel"
    make -s ARCH="$RACH2_ARCH" rach2
}

# Put back the saved krandom.c, touch it, and mark it unchanged.
restore_kernel()
{
    cp "$work/krandom.saved" "$krandom"
    touch "$krandom"
    changed=
}

# Stop the boot, put back the private kcpu.c, and when the kernel's fault is
# in the tree, restore it and rebuild. The work directory is removed too,
# unless the proof failed after printing its path, which sets `keep`; a
# signal clears `keep`.
cleanup()
{
    exit_status=$?
    stop_child
    if [ -n "$work" ] && [ -f "$work/saved.c" ]; then
        cp "$work/saved.c" "$source"
    fi
    if [ -n "$changed" ]; then
        restore_kernel
        rebuild >"$work/cleanup.build.log" 2>&1
    fi
    if [ "$exit_status" -eq 0 ] || [ -z "$keep" ]; then
        rm -rf ${work:+"$work"}
    fi
}
trap cleanup EXIT
trap 'keep=; exit 129' HUP
trap 'keep=; exit 130' INT
trap 'keep=; exit 143' TERM
work=$(mktemp -d "$repo/build/review-proof/rng.XXXXXX")
mkdir -p "$work/rach2/sys/lib" "$work/rach2/sys/arch/x86" "$work/tests" "$work/utils" \
    "$work/tools/etude/include/etude"
cp rach2/sys/arch/x86/kcpu.c rach2/sys/arch/x86/kcpu.h "$work/rach2/sys/arch/x86/"
cp rach2/sys/lib/kint.h "$work/rach2/sys/lib/"
cp tests/rngprobe.c tests/probe.h tests/probe_report.h "$work/tests/"
cp tools/etude/include/etude/report.h "$work/tools/etude/include/etude/"
cp utils/pingcall.h "$work/utils/"
source="$work/rach2/sys/arch/x86/kcpu.c"
cp "$source" "$work/saved.c"
cp "$krandom" "$work/krandom.saved"
echo "rng-mutation: $RACH2_ARCH: logs: $work"
keep=1
make -s ARCH="$RACH2_ARCH" rach2 build/tests/system/harness build/krffs/mkfs.krffs \
    build/krffs/stageimage.krffs build/krffs/readfile.krffs "$casecheck" >"$work/build.log" 2>&1 ||
    { cat "$work/build.log"; exit 1; }
cpu=$("$casecheck" processor max)

# run_phase PHASE STATUS MATCH: build the probe, stage it in an image, boot
# it under the profile max, and judge its records, which must give the
# casecheck status STATUS and, when MATCH is not empty, a line with MATCH.
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
    cp "$kernel" "$work/$phase.kernel"
    build/tests/system/harness --kernel "$kernel" \
        --initrd "$work/$phase.krffs" --append 'rach2.nonet rach2.noaudio' \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg="$cpu" \
        --log "$work/$phase.console.log" --timeout 30 --ok '@etude 1 end ' \
        "expect=Type 'help'" 'send=exec /rngprobe' 'expect=@etude 1 end ' \
        >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || :
    child=
    cat "$work/$phase.log"
    tr -d '\r' < "$work/$phase.console.log" | grep -a '^@etude 1 ' > "$work/$phase.records" || :
    judged=0
    "$casecheck" report "$manifest" guest.default.rngprobe "$RACH2_ARCH" qemu \
        "$work/$phase.records" >"$work/$phase.judged" 2>&1 || judged=$?
    cat "$work/$phase.judged"
    [ "$judged" -eq "$2" ] ||
        { echo "rng-mutation: $phase: casecheck status $judged, not $2" >&2; exit 1; }
    [ -z "$3" ] || grep -F -- "$3" "$work/$phase.judged" >/dev/null ||
        { echo "rng-mutation: $phase: the report failed for another reason" >&2; exit 1; }
    echo "rng-mutation: result: $phase: casecheck status $judged"
}

run_phase fixed 0 'branch source trusted decided by feature'
once "$source" '"+c"(rc)'
sed 's/"+c"(rc)/"=c"(rc)/' "$work/saved.c" > "$source"
run_phase cpuid 1 'assert cpuid-leaf7 fail'
cp "$work/saved.c" "$source"
once "$krandom" "$trust"
changed=1
sed "s/$trust/$distrust/" "$work/krandom.saved" > "$krandom"
rebuild >"$work/distrust.build.log" 2>&1 || { cat "$work/distrust.build.log"; exit 1; }
run_phase distrust 1 'the program took fail-closed where the processor gives trusted'
grep -a 'RNGPROBE OK (fail-closed)' "$work/distrust.console.log" >/dev/null ||
    { echo "rng-mutation: distrust: the probe did not pass by its fail-closed branch" >&2; exit 1; }
restore_kernel
rebuild >"$work/restored.build.log" 2>&1 || { cat "$work/restored.build.log"; exit 1; }
run_phase restored 0 'outcome pass'
cmp "$work/saved.c" "$source"
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo "rng-mutation: $RACH2_ARCH: result: pass, both faults failed the report and the restored kernel matched"
