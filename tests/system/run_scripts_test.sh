#!/bin/sh
#
# run_scripts_test.sh: the command lines the run scripts under rach2/ give
# QEMU.
#
# run.sh, run_from_iso.sh and run_from_disk.sh each boot with `-cpu max`
# unless CPU is set to another model, because only a CPU with RDSEED or
# RDRAND gives the kernel trusted entropy: without one, getrandom and the
# random devices fail closed and dropbear can make no host key. The rows
# require that default and the override of each script, and the audio,
# network, disk-bus and rescue options of run.sh. Copies of the scripts run
# in a private tree against a stand-in qemu-system-i386 on the PATH, which
# writes its arguments one per line, so nothing boots.
#
# With --mutate each fault below is put into the private copy of one script,
# must fail the row named for it, and is restored and touched before every row
# runs again.
#
# Usage: sh tests/system/run_scripts_test.sh [--mutate]
#
set -eu

case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    *) echo 'run-scripts-test: usage: run_scripts_test.sh [--mutate]' >&2; exit 2 ;;
esac

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/run-scripts.XXXXXX")
trap 'rm -rf "$work"' EXIT
# The scripts name their files from `cd` and `pwd`, which fold a doubled
# slash, so the expected paths are made the same way.
work=$(CDPATH='' cd -- "$work" && pwd)
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$work/rach2" "$work/build/rach2" "$work/build/images" "$work/bin" "$work/saved"
for f in run.sh run_from_disk.sh run_from_iso.sh qemu_devices.sh; do
    cp "$repo/rach2/$f" "$work/rach2/$f"
    cp "$repo/rach2/$f" "$work/saved/$f"
done
: >"$work/build/rach2/kernel"
: >"$work/build/images/test.krffs"
: >"$work/build/images/os-real-grub.disk"
: >"$work/build/images/test.iso"
: >"$work/disk.img"
cat >"$work/bin/qemu-system-i386" <<'EOF'
#!/bin/sh
for a in "$@"; do printf '%s\n' "$a"; done >"$ARGS_OUT"
EOF
chmod +x "$work/bin/qemu-system-i386"
images="$work/rach2/../build/images"

pass()
{
    echo "run-scripts-test: result: pass: $1"
}

fail()
{
    echo "run-scripts-test: result: fail: $1: $2"
    failed="$failed $1"
}

# run NAME [VAR=VALUE...] -- SCRIPT [ARG...]: run a copied script with the
# stand-in QEMU and nothing of the caller's settings, its arguments to QEMU in
# $work/NAME.args, its output in $work/NAME.out and its status in $rc.
run()
{
    name=$1
    shift
    settings=
    while [ "$1" != -- ]; do
        settings="$settings $1"
        shift
    done
    shift
    ran=$1
    shift
    rm -f "$work/$name.args"
    rc=0
    # shellcheck disable=SC2086 # one setting a word, split on purpose
    env -u CPU -u NET -u NIC -u NET_HOSTFWD -u DISK -u DISKBUS -u DISKNOMOUNT \
        -u RESCUE -u AUDIO_DEVICE PATH="$work/bin:$PATH" ARGS_OUT="$work/$name.args" \
        $settings sh "$work/rach2/$ran" "$@" >"$work/$name.out" 2>&1 || rc=$?
}

# row NAME ARG...: every ARG is one argument QEMU was given.
row()
{
    name=$1
    shift
    missing=
    for want in "$@"; do
        grep -Fxq -- "$want" "$work/$name.args" 2>/dev/null || missing="$missing '$want'"
    done
    if [ "$rc" -eq 0 ] && [ -z "$missing" ]; then
        pass "$name"
    else
        fail "$name" "status $rc, no argument$missing"
    fi
}

# absent NAME TEXT: no argument QEMU was given contains TEXT.
absent()
{
    if [ "$rc" -eq 0 ] && [ -f "$work/$1.args" ] && ! grep -Fq -- "$2" "$work/$1.args"; then
        pass "$1"
    else
        fail "$1" "status $rc, or an argument has '$2'"
    fi
}

# refused NAME STATUS TEXT: the script stopped with STATUS before QEMU ran and
# said TEXT.
refused()
{
    if [ "$rc" -eq "$2" ] && [ ! -f "$work/$1.args" ] && grep -Fq -- "$3" "$work/$1.out"; then
        pass "$1"
    else
        fail "$1" "status $rc: $(cat "$work/$1.out")"
    fi
}

rows()
{
    failed=
    run run-default -- run.sh test
    row run-default -cpu max -kernel "$work/rach2/../build/rach2/kernel" \
        -initrd "$images/test.krffs" -machine pcspk-audiodev=audio0 \
        -netdev user,id=net0,hostfwd=tcp::8080-:80 -device rtl8139,netdev=net0
    run run-cpu CPU=qemu64 -- run.sh test
    row run-cpu -cpu qemu64
    run run-sb16 -- run.sh test sb16
    row run-sb16 -device sb16,audiodev=audio0
    run run-hda -- run.sh test hda
    row run-hda -device intel-hda -device hda-duplex,audiodev=audio0
    run run-nonet NET=off -- run.sh test
    absent run-nonet -netdev
    run run-e1000 NIC=e1000 -- run.sh test
    row run-e1000 -device e1000,netdev=net0
    run run-ide DISK="$work/disk.img" -- run.sh test
    row run-ide "file=$work/disk.img,format=raw,if=ide,index=0,media=disk" -append rach2.fsdisk
    run run-ahci DISK="$work/disk.img" DISKBUS=ahci -- run.sh test
    row run-ahci -device ich9-ahci,id=ahci0 -append "rach2.disk=ahci rach2.fsdisk"
    run run-nomount DISK="$work/disk.img" DISKNOMOUNT=1 -- run.sh test
    absent run-nomount rach2.fsdisk
    run run-rescue RESCUE=1 -- run.sh test
    row run-rescue -append rach2.rescue
    run run-missing -- run.sh os-simple
    refused run-missing 1 "build it with 'make os-simple-image'"
    run disk-default -- run_from_disk.sh
    row disk-default -cpu max \
        "file=$images/os-real-grub.disk,format=raw,if=ide,index=0,media=disk" \
        -netdev user,id=net0,hostfwd=tcp::2222-:22
    run disk-cpu CPU=qemu64 -- run_from_disk.sh
    row disk-cpu -cpu qemu64
    run iso-default -- run_from_iso.sh test
    row iso-default -cpu max -cdrom "$images/test.iso"
    run iso-cpu CPU=qemu64 -- run_from_iso.sh test
    row iso-cpu -cpu qemu64
    run iso-usage -- run_from_iso.sh
    refused iso-usage 2 'usage: ./run_from_iso.sh'
}

# Change one private copy where the old text occurs exactly once.
change()
{
    python3 - "$work/rach2/$1" "$2" "$3" <<'PY'
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit("run-scripts-test: pattern is not unique: " + old)
path.write_text(text.replace(old, new))
PY
    touch "$work/rach2/$1"
}

rows
[ -z "$failed" ] || exit 1
echo "run-scripts-test: result: pass: every row"
[ "$mutate" -eq 1 ] || exit 0

# The dollars are the scripts' own, left for their shell.
# shellcheck disable=SC2016
for fault in run-default run-override disk-default disk-override iso-default iso-override; do
    case $fault in
        run-*) copy=run.sh ;;
        disk-*) copy=run_from_disk.sh ;;
        iso-*) copy=run_from_iso.sh ;;
    esac
    case $fault in
        *-default)
            change "$copy" 'CPU="${CPU:-max}"' 'CPU="${CPU:-qemu64}"'
            named=${fault%-default}-default ;;
        *-override)
            change "$copy" 'CPU="${CPU:-max}"' 'CPU=max'
            named=${fault%-override}-cpu ;;
    esac
    rows >"$work/$fault.log"
    case " $failed " in
        *" $named "*) echo "run-scripts-test: result: $fault: the fault fails $named" ;;
        *)
            cat "$work/$fault.log"
            echo "run-scripts-test: result: fail: the $fault fault passed $named" >&2
            exit 1 ;;
    esac
    cp "$work/saved/$copy" "$work/rach2/$copy"
    touch "$work/rach2/$copy"
    rows >"$work/$fault.restored.log"
    [ -z "$failed" ] || { cat "$work/$fault.restored.log"; exit 1; }
    echo "run-scripts-test: result: $fault: restored, every row passes"
done
echo "run-scripts-test: result: pass: six faults failed their rows; restored rows passed"
