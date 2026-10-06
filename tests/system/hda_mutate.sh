#!/bin/sh
# Boot the staged diagnostic and prove that the proc path is required, on the
# kernel, programs and emulator of the architecture ARCH names.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" hda-guest "$repo/tests/system/hda_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/hda-guest.XXXXXX")
source=rach2/sys/fs/synthfs.c
cp "$source" "$work/source.saved"
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do sleep 1; done
        child=
    fi
}

rebuild()
{
    rm -f "$tree"/rach2/sys/fs/synthfs.o "$tree"/rach2/kernel
    make -s rach2 "ARCH=$RACH2_ARCH"
}

restore()
{
    cp "$work/source.saved" "$source"
    touch "$source"
    changed=
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        rebuild >"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    expected=$2
    append='rach2.rescue rach2.nonet'
    command='exec /hdaprobe'
    if [ "$phase" = absent ]; then
        append="$append rach2.noaudio"
        command="$command absent"
    fi
    rebuild >"$work/$phase.build.log" 2>&1
    cat "$work/$phase.build.log"
    cp "$tree"/rach2/kernel "$work/$phase.kernel"
    cksum "$source" "$tree"/rach2/kernel "$work/test.krffs" >"$work/$phase.artifacts"
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$work/$phase.kernel" --initrd "$work/test.krffs" \
        --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg=max \
        --qemu-arg=-device --qemu-arg=intel-hda \
        --qemu-arg=-device --qemu-arg=hda-duplex,audiodev=au0 \
        --qemu-arg=-audiodev --qemu-arg="wav,id=au0,path=$work/$phase.wav" \
        --append "$append" --timeout 45 --steptmo 20 \
        --ok 'HDAPROBE OK' --never 'HDAPROBE FAIL' \
        --log "$work/$phase.serial.log" \
        "expect=Type 'help'" "send=$command" 'expect=HDAPROBE' \
        'expect=returning to kernel shell' >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.log" "$work/$phase.serial.log"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] &&
            grep -Fq 'HDAPROBE FAIL: open /proc/audio' "$work/$phase.serial.log" || exit 1
    fi
    echo "hda-guest: result: $phase status $status"
}

echo "hda-guest: logs: $work"
# The probe is one of the musl programs, built by `make musl-programs` with
# the others, so this script neither compiles it nor keeps a copy of that
# compile line.
{
    make -s check-libc build/tests/system/harness build/tests/system/runutil "ARCH=$RACH2_ARCH"
    make -s -C utils ../"$tree"/utils/audiotest "ARCH=$RACH2_ARCH"
    make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
        ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs "ARCH=$RACH2_ARCH"
    make -s musl-programs "ARCH=$RACH2_ARCH"
} >"$work/build.log" 2>&1
cp "$tree"/tests/musl/hdaprobe "$work/hdaprobe"
for profile in rach2-metal rach2-metal-disk os-real; do
    grep -Fx 'file 0755 /bin/audiotest build/utils/audiotest' \
        "krffs/manifests/$profile.manifest" >/dev/null
done
{
    grep -Fx 'file 0755 /bin/audiotest build/utils/audiotest' krffs/manifests/rach2-metal.manifest
    echo "file 0755 /hdaprobe $work/hdaprobe"
} >"$work/test.manifest"
{
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=2
    build/krffs/mkfs.krffs "$work/test.krffs"
    build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
} >>"$work/build.log" 2>&1
cat "$work/build.log"
for program in audiotest hdaprobe; do
    guest=/hdaprobe
    original="$work/hdaprobe"
    if [ "$program" = audiotest ]; then
        guest=/bin/audiotest
        original="$tree"/utils/audiotest
    fi
    build/krffs/readfile.krffs "$work/test.krffs" "$guest" "$work/$program.staged"
    cmp "$original" "$work/$program.staged"
done
run_phase fixed pass
changed=1
python3 - "$source" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
old = '"audio", VNODE_PROC_AUDIO'
if s.count(old) != 1:
    raise SystemExit('hda-guest: audio path is not unique')
p.write_text(s.replace(old, '"audio-disabled", VNODE_PROC_AUDIO'))
PY
run_phase mutated fail
if cmp -s "$work/fixed.kernel" "$work/mutated.kernel"; then
    echo 'hda-guest: mutation did not change kernel' >&2; exit 1
fi
restore
run_phase restored pass
cmp "$work/fixed.kernel" "$work/restored.kernel"
run_phase absent pass
echo 'hda-guest: result: pass: compiled fault failed and restored kernel matched'
