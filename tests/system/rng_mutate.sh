#!/bin/sh
# Recompile the production CPUID helper in the RNG probe for every phase.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
for artifact in build/rach2/kernel build/tests/system/harness \
    build/krffs/mkfs.krffs build/krffs/stageimage.krffs build/krffs/readfile.krffs; do
    [ -x "$artifact" ] || { echo "rng-mutation: missing $artifact" >&2; exit 1; }
done
mkdir -p build/review-proof
work=$(mktemp -d "$repo/build/review-proof/rng.XXXXXX")
mkdir -p "$work/rach2/sys/lib" "$work/tests"
cp rach2/sys/lib/kcpu.c rach2/sys/lib/kcpu.h rach2/sys/lib/kint.h "$work/rach2/sys/lib/"
cp tests/rngprobe.c "$work/tests/"
source="$work/rach2/sys/lib/kcpu.c"
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
    "${CROSS_CC:-i686-elf-gcc}" -m32 -nostdlib -static -ffreestanding \
        -Wl,-Ttext=0x08048000 -Wl,--build-id=none \
        -o "$binary" "$work/tests/rngprobe.c" >"$work/$phase.compile.log" 2>&1
    printf 'file 0755 /rngprobe %s\n' "$binary" > "$work/rng-proof.manifest"
    sh krffs/mkimage.sh "$work/rng-proof.manifest" "$work/$phase.krffs" 1 \
        >"$work/$phase.image.log" 2>&1
    build/krffs/readfile.krffs "$work/$phase.krffs" /rngprobe "$work/$phase.extracted"
    cmp "$binary" "$work/$phase.extracted"
    build/tests/system/harness --kernel build/rach2/kernel \
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
[ "$(grep -c '\"+c\"(rc)' "$source")" -eq 1 ]
sed 's/"+c"(rc)/"=c"(rc)/' "$work/saved.c" > "$source"
run_phase mutated
restore
run_phase restored
cmp "$work/saved.c" "$source"
