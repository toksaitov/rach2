#!/bin/sh
# Each kernel, probe and disk is built or copied into private scratch.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# line_once FILE TEXT: TEXT, the line a fault changes, is once in FILE.
line_once()
{
    if [ "$(grep -c "$2" "$1" 2>/dev/null)" != 1 ]; then
        echo "usbdisk-mutation: not once in $1: $2" >&2
        exit 1
    fi
}

if [ "${1:-}" = --texts ]; then
    line_once rach2/sys/fs/vfs.c 'm = rc == -EIO ?'
    line_once rach2/sys/fs/vfs.c 'return mnt->dev &&'
    echo 'usbdisk-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" usbdisk-mutation "$repo/tests/system/usbdisk_mutate.sh" "$@"
# The architecture, from ARCH as mk/arch.sh reads it: its compiler, its text
# address, its build tree and its emulator.
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU
stick=build$ARCH_SUFFIX/images/usbdisk/stick-mbr.img
harness="$repo/build/tests/system/harness"
for artifact in "$harness" "$stick" \
    build/tools/inspect/elfcheck build/krffs/mkfs.krffs \
    build/krffs/stageimage.krffs build/krffs/readfile.krffs \
    build/tools/inspect/setidcheck; do
    if [ ! -f "$artifact" ]; then
        echo "usbdisk-mutation: missing $artifact; run tests/run usbdisk first" >&2
        exit 1
    fi
done

failure_pattern()
{
    if [ "$fault" = nested ]; then
        echo '^USBDISKPROBE FAIL: the removed child mount did not retain its open description'
    else
        echo '^USBDISKPROBE FAIL: (the removed mount still resolves|a cached directory listed after removal)'
    fi
}

resume=0
fault=gone
if [ "$#" -eq 2 ] && { [ "$1" = --resume ] || [ "$1" = --restore ]; }; then
    work=$2
    fault=$(cat "$work/fault")
    case $fault in gone|nested) ;; *) exit 1 ;; esac
    resume=1
    [ "$1" != --restore ] || resume=2
    for mode in holdnested holdnestedexfat; do
        grep -aFqx "HARNESS: saw 'USBDISKPROBE OK'" "$work/fixed.$mode.log" || {
            echo "usbdisk-mutation: no passing fixed $mode log in $work" >&2
            exit 1
        }
    done
    [ -f "$work/vfs.saved" ] && [ -f "$work/fixed.kernel" ] || exit 1
    cmp tests/usbdiskprobe.c "$work/usbdiskprobe.c"
    cmp rach2/sys/fs/vfs.c "$work/vfs.saved"
    (cd "$work" && shasum -a 256 -c proof.sha256)
    if [ "$resume" -eq 2 ]; then
        for mode in holdnested holdnestedexfat; do
            if ! grep -aEq "$(failure_pattern)" "$work/mutated.$mode.log" ||
                ! grep -aFqx "HARNESS: saw 'USBDISKPROBE FAIL', which must not appear" \
                    "$work/mutated.$mode.log"; then
                echo "usbdisk-mutation: no expected mutant $mode failure in $work" >&2
                exit 1
            fi
        done
    fi
elif [ "$#" -eq 0 ] || { [ "$#" -eq 1 ] && [ "$1" = --nested ]; }; then
    work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-usbdisk.XXXXXX")
    [ "$#" -eq 0 ] || fault=nested
else
    echo 'usbdisk-mutation: usage: [--nested | --resume DIRECTORY | --restore DIRECTORY | --texts]' >&2
    exit 2
fi
runner="$work/runutil"
child=
recorder=
source="$work/rach2/sys/fs/vfs.c"
if [ "$resume" -eq 0 ]; then
    cp -R rach2 mk "$work/"
    # The kernel's link asks the directory above rach2/ for the ELF checker;
    # the private tree's has a rule with nothing to do, and ELFCHECK names the
    # built one.
    printf 'build/tools/inspect/elfcheck: ;\n' >"$work/Makefile"
    cp "$source" "$work/vfs.saved"
    cp tests/usbdiskprobe.c "$work/usbdiskprobe.c"
    printf '%s\n' "$fault" > "$work/fault"
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
    child=
    recorder=
    rm -f "$work/output"
    cp "$work/vfs.saved" "$source"
    touch "$source"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"${CC:-cc}" -O2 -Wall -Wextra -o "$runner" tests/system/runner/runutil.c

build_probe()
{
    # shellcheck disable=SC2086 # ARCH_LDFLAGS is empty or one flag
    "${CROSS_CC:-${CROSS}gcc}" "$MFLAG" -nostdlib -static -ffreestanding \
        -ansi -Werror -Wdeclaration-after-statement -I "$repo/tests" \
        -Wl,-Ttext="$ARCH_USER_TEXT" -Wl,--build-id=none $ARCH_LDFLAGS \
        -o "$work/usbdiskprobe" "$work/usbdiskprobe.c"
    awk -v probe="$work/usbdiskprobe" '
        $1 == "file" && $3 == "/usbdiskprobe" {$4 = probe}
        {print}
    ' "$repo/krffs/manifests/usbdisk-proof.manifest" > "$work/usbdisk-proof.manifest"
    dd if=/dev/zero of="$work/test.krffs" bs=1048576 count=4 2> "$work/image.log"
    "$repo/build/krffs/mkfs.krffs" "$work/test.krffs" >> "$work/image.log"
    "$repo/build/krffs/stageimage.krffs" "$work/usbdisk-proof.manifest" "$work/test.krffs" >> "$work/image.log"
    "$repo/build/tools/inspect/setidcheck" "$work/usbdisk-proof.manifest"
    "$repo/build/tools/inspect/setidcheck" --image "$work/test.krffs" --manifest "$work/usbdisk-proof.manifest"
    "$repo/build/krffs/readfile.krffs" "$work/test.krffs" /usbdiskprobe "$work/extracted" >> "$work/image.log"
    cmp "$work/usbdiskprobe" "$work/extracted"
}

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
        -j4 "ARCH=$RACH2_ARCH" "ELFCHECK=$repo/build/tools/inspect/elfcheck"
    cp "$work/build$ARCH_SUFFIX/rach2/kernel" "$work/$phase.kernel"
    if [ "$phase" = fixed ]; then
        (cd "$work" && shasum -a 256 fault usbdiskprobe.c usbdiskprobe \
            test.krffs vfs.saved fixed.kernel > proof.sha256)
    fi
    shasum -a 256 "$work/$phase.kernel"
    for mode in holdnested holdnestedexfat; do
        row="$work/$phase.$mode"
        cp "$stick" "$row.img"
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
            'expect=USBDISKPROBE HOLDING' 'usb=del:st0' \
            'expect=USBDISKPROBE ' || status=$?
        if [ "$phase" = mutated ]; then
            [ "$status" -eq 1 ] && grep -aEq \
                "$(failure_pattern)" \
                "$row.log" || exit 1
        else
            [ "$status" -eq 0 ] || exit 1
        fi
        echo "usbdisk-mutation: result: $phase $mode status $status"
    done
}

echo "usbdisk-mutation: logs: $work"
if [ "$resume" -eq 0 ]; then
    build_probe
    run_phase fixed
fi
if [ "$resume" -ne 2 ]; then
    if [ "$fault" = nested ]; then
        line_once "$work/vfs.saved" 'm = rc == -EIO ?'
        sed 's/m = rc == -EIO ?/m = rc == -EIO \&\& 0 ?/' "$work/vfs.saved" > "$source"
    else
        line_once "$work/vfs.saved" 'return mnt->dev &&'
        sed '/return mnt->dev &&/c\
    (void)mnt;\
    return 0;
' "$work/vfs.saved" > "$source"
    fi
    touch "$source"
    run_phase mutated
fi
cleanup
run_phase restored
cmp "$work/vfs.saved" "$source"
cmp "$work/fixed.kernel" "$work/restored.kernel"
echo 'usbdisk-mutation: result: fixed and restored kernels match'
