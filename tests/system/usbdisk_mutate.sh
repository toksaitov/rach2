#!/bin/sh
# Each kernel and disk is private. The test image is the one just proved.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
harness="$repo/build/tests/system/harness"
for artifact in "$harness" build/images/test.krffs \
    build/images/usbdisk/stick-mbr.img build/tools/inspect/elfcheck; do
    if [ ! -f "$artifact" ]; then
        echo "usbdisk-mutation: missing $artifact; run tests/run usbdisk first" >&2
        exit 1
    fi
done

resume=0
if [ "$#" -eq 2 ] && { [ "$1" = --resume ] || [ "$1" = --restore ]; }; then
    work=$2
    resume=1
    [ "$1" != --restore ] || resume=2
    for mode in hold holdexfat; do
        grep -aFqx "HARNESS: saw 'USBDISKPROBE OK'" "$work/fixed.$mode.log" || {
            echo "usbdisk-mutation: no passing fixed $mode log in $work" >&2
            exit 1
        }
    done
    [ -f "$work/vfs.saved" ] && [ -f "$work/fixed.kernel" ] || exit 1
    if [ "$resume" -eq 2 ]; then
        for mode in hold holdexfat; do
            grep -aEq '^USBDISKPROBE FAIL: (the removed mount still resolves|a cached directory listed after removal)' \
                "$work/mutated.$mode.log" &&
                grep -aFqx "HARNESS: saw 'USBDISKPROBE FAIL', which must not appear" \
                "$work/mutated.$mode.log" || {
                    echo "usbdisk-mutation: no expected mutant $mode failure in $work" >&2
                    exit 1
                }
        done
    fi
elif [ "$#" -eq 0 ]; then
    work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-usbdisk.XXXXXX")
else
    echo 'usbdisk-mutation: usage: [--resume WORK-DIRECTORY | --restore WORK-DIRECTORY]' >&2
    exit 2
fi
runner="$work/runutil"
child=
recorder=
source="$work/rach2/sys/fs/vfs.c"
if [ "$resume" -eq 0 ]; then
    cp -R rach2 mk "$work/"
    cp build/images/test.krffs "$work/test.krffs"
    cp "$source" "$work/vfs.saved"
fi

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ -n "$recorder" ]; then
        wait "$recorder" 2>/dev/null || :
    fi
    cp "$work/vfs.saved" "$source"
    touch "$source"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"${CC:-cc}" -O2 -Wall -Wextra -o "$runner" tests/system/runner/runutil.c

run_logged()
{
    log=$1
    shift
    mkfifo "$work/output" || exit 1
    tee "$log" < "$work/output" &
    recorder=$!
    "$runner" watch usbdisk-mutation 120 "$log.jobs" "$@" \
        > "$work/output" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    wait "$recorder" || exit 1
    recorder=
    rm "$work/output"
    return "$status"
}

run_phase()
{
    phase=$1
    echo "usbdisk-mutation: building $phase"
    run_logged "$work/$phase.build.log" make -s -C "$work/rach2" \
        -j4 "ELFCHECK=$repo/build/tools/inspect/elfcheck"
    cp "$work/build/rach2/kernel" "$work/$phase.kernel"
    shasum -a 256 "$work/$phase.kernel"
    for mode in hold holdexfat; do
        row="$work/$phase.$mode"
        cp build/images/usbdisk/stick-mbr.img "$row.img"
        echo "usbdisk-mutation: running $phase $mode"
        status=0
        run_logged "$row.log" "$harness" \
            --kernel "$work/$phase.kernel" --initrd "$work/test.krffs" \
            --qemu-arg=-cpu --qemu-arg=max \
            --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
            --qemu-arg=-device --qemu-arg=usb-kbd,bus=xhci.0 \
            --qemu-arg=-device --qemu-arg=usb-mouse,bus=xhci.0 \
            --drive "if=none,id=stick,format=raw,file=$row.img" \
            --qemu-arg=-device \
            --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
            --timeout 90 --steptmo 40 --log /dev/stdout \
            --ok 'USBDISKPROBE OK' --never 'USBDISKPROBE FAIL' \
            "expect=Type 'help'" "send=exec /usbdiskprobe $mode" \
            'expect=USBDISKPROBE HOLDING' wait=1 'usb=del:st0' \
            'expect=USBDISKPROBE ' || status=$?
        if [ "$phase" = mutated ]; then
            [ "$status" -eq 1 ] && grep -aEq \
                '^USBDISKPROBE FAIL: (the removed mount still resolves|a cached directory listed after removal)' \
                "$row.log" || exit 1
        else
            [ "$status" -eq 0 ] || exit 1
        fi
        echo "usbdisk-mutation: result: $phase $mode status $status"
    done
}

echo "usbdisk-mutation: logs: $work"
if [ "$resume" -eq 0 ]; then
    run_phase fixed
fi
if [ "$resume" -ne 2 ]; then
    [ "$(grep -c 'return mnt->dev &&' "$work/vfs.saved")" -eq 1 ]
    sed '/return mnt->dev &&/c\
    (void)mnt;\
    return 0;
' "$work/vfs.saved" > "$source"
    touch "$source"
    run_phase mutated
fi
cleanup
run_phase restored
cmp "$work/vfs.saved" "$source"
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo 'usbdisk-mutation: result: fixed and restored kernels match'
