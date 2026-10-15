#!/bin/sh
# install_stick_test.sh STICK DISK ROOT WORK: check an install stick in QEMU,
# with a copy of the sealed disk DISK as its target.
#
# The stick's exFAT volume must have the root image ROOT byte for byte and a
# manifest that names DISK. Three boots from the stick follow. The first, by
# USB, must mount the volume and see both images. The second, by IDE, saves
# the target's boot and recovery partitions to the volume, updates the
# target's root from the stick with `rach2-install --update`, which must keep
# the machine's groups and replace its files and set the next boot to the
# candidate, and then zeroes both partitions. The third writes the saved
# copies back. Both partitions must then be as they were and the saved
# copies the same, and the volume must check clean. A `STICKFAIL` line fails
# a boot. WORK keeps the logs.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
stick=$1
disk=$2
root=$3
work=$4
mkdir -p "$work"
work=$(CDPATH='' cd -- "$work" && pwd)
fixture="$repo/build/tests/host/mkexfatfixture"
harness="$repo/build/tests/system/harness"
runutil="$repo/build/tests/system/runutil"
child=

# Stop the running boot's process group and wait until it is gone.
stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do sleep 1; done
        child=
    fi
}
trap stop_child EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Report $1 and exit 1.
fail()
{
    echo "install-stick-test: $1" >&2
    exit 1
}

# boot PHASE BUS TOKEN: boot the stick on BUS, usb or ide, with the target on
# IDE, run the steps of PHASE.steps, and require TOKEN.
boot()
{
    phase=$1
    bus=$2
    token=$3
    limit=600
    [ "$bus" != usb ] || limit=30
    set -- "$harness" --bios --net hubport,id=net0,hubid=0 \
        --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 \
        --drive "if=none,id=stick,format=raw,file=$work/stick.img" \
        --qemu-arg=-drive --qemu-arg="file=$work/target.img,format=raw,if=ide,index=0" \
        --timeout 1800 --steptmo "$limit" --never STICKFAIL \
        --ok "$token" --log "$work/$phase.serial.log"
    if [ "$bus" = usb ]; then
        set -- "$@" --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
            --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,bootindex=0
    else
        set -- "$@" --qemu-arg=-device \
            --qemu-arg=ide-hd,drive=stick,bus=ide.0,unit=1,bootindex=0
    fi
    set -- "$@" 'expect=recovery image' 'expect=# '
    while IFS= read -r step; do set -- "$@" "$step"; done < "$work/$phase.steps"
    "$runutil" group "$@" >"$work/$phase.harness.log" 2>&1 &
    child=$!
    printf '%s\n' "$child" >"$work/$phase.pid"
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.harness.log" "$work/$phase.serial.log"
    [ "$status" -eq 0 ] || fail "$phase failed: status $status"
    echo "install-stick-test: result: $phase passed"
}

echo "install-stick-test: logs: $work"
start=$(od -An -tu4 -j 470 -N 4 "$stick" | tr -d ' ')
root_name=$(basename "$root")
root_bytes=$(wc -c < "$root" | tr -d ' ')
"$fixture" --check "$stick" "$start" --read "/$root_name" >"$work/root.actual" \
    2>"$work/root.check.log" || fail 'missing root image'
cmp "$root" "$work/root.actual" || fail 'root image differs'
rm "$work/root.actual"
"$fixture" --check "$stick" "$start" --read /rach2-install.manifest \
    >"$work/manifest" 2>"$work/manifest.check.log"
disk_name=$(basename "$disk")
grep -Fx "image $disk_name" "$work/manifest" >/dev/null || fail 'wrong sealed disk name'
# The sealed disk's boot and recovery partitions, entries 1 and 2 of its GUID
# table, which `mkgpt --list` prints in bytes.
"$repo/build/tools/image/mkgpt" --list "$disk" >"$work/table" || fail 'the sealed disk has no GUID table'
for p in 1 2; do
    first=$(awk -v n="$p" '$1 == n { printf "%d", $3 / 512 }' "$work/table")
    count=$(awk -v n="$p" '$1 == n { printf "%d", $4 / 512 }' "$work/table")
    [ "${count:-0}" -eq 131072 ] && [ $((first % 2048)) -eq 0 ] || fail 'expected two aligned 64 MiB partitions'
    dd if="$disk" of="$work/p$p.before" bs=1048576 skip=$((first / 2048)) count=64 2>"$work/p$p.read.log"
    printf '%s\n' "$((first / 2048))" >"$work/p$p.offset"
done
boot_offset=$(cat "$work/p1.offset")
recovery_offset=$(cat "$work/p2.offset")
cp "$stick" "$work/stick.img"
cp "$disk" "$work/target.img"
cat >"$work/usb.steps" <<EOF
send=mkdir -p /stick && mount -t exfat /dev/disk1p2 /stick && test -f /stick/$disk_name && test \$(stat -c %s /stick/$root_name) = $root_bytes && echo USB""FILES || { echo STICK""FAIL: usb files; poweroff -f; }
expect=USBFILES
send=umount /stick && echo USB""DONE || { echo STICK""FAIL: usb unmount; poweroff -f; }
expect=USBDONE
EOF
boot usb usb USBDONE
cat >"$work/update.steps" <<EOF
send=mkdir -p /stick /old && mount -t exfat /dev/disk1p2 /stick && echo STICK""MOUNTED || { echo STICK""FAIL: mount; poweroff -f; }
expect=STICKMOUNTED
send=df -k /stick; test \$(df -k /stick | awk 'END {print \$4}') -ge 131072 && echo SPACE""OK || { echo STICK""FAIL: space; poweroff -f; }
expect=SPACEOK
send=mkdir /stick/saved && dd if=/dev/disk0 of=/stick/saved/boot.img bs=1M skip=$boot_offset count=64 && test \$(stat -c %s /stick/saved/boot.img) = 67108864 && echo BOOT""SAVED || { echo STICK""FAIL: boot save; poweroff -f; }
expect=BOOTSAVED
send=dd if=/dev/disk0 of=/stick/saved/recovery.img bs=1M skip=$recovery_offset count=64 && test \$(stat -c %s /stick/saved/recovery.img) = 67108864 && echo RECOVERY""SAVED || { echo STICK""FAIL: recovery save; poweroff -f; }
expect=RECOVERYSAVED
send=mount -t ext2 /dev/disk0p3 /old && echo stickproof:x:1234: >> /old/etc/group && echo old-root > /old/stick-before && umount /old && echo ROOT""CHANGED || { echo STICK""FAIL: root setup; poweroff -f; }
expect=ROOTCHANGED
send=rach2-install --update /stick/$root_name /dev/disk0 && echo UPDATE""OK || { echo STICK""FAIL: update; poweroff -f; }
expect=UPDATEOK
send=mount -t ext2 /dev/disk0p3 /old && test ! -e /old/stick-before && grep -qx stickproof:x:1234: /old/etc/group && umount /old && echo ROOT""CHECKED || { echo STICK""FAIL: root check; poweroff -f; }
expect=ROOTCHECKED
send=bootctl --target /dev/disk0p1 status
expect=saved_entry: recovery
expect=next_entry: candidate
send=dd if=/dev/zero of=/dev/disk0 bs=1M seek=$boot_offset count=64 conv=notrunc && dd if=/dev/zero of=/dev/disk0 bs=1M seek=$recovery_offset count=64 conv=notrunc && sync && umount /stick && echo DAMAGE""DONE || { echo STICK""FAIL: damage; poweroff -f; }
expect=DAMAGEDONE
EOF
boot update ide DAMAGEDONE
for p in 1 2; do
    offset=$(cat "$work/p$p.offset")
    dd if="$work/target.img" of="$work/p$p.damaged" bs=1048576 skip="$offset" count=64 2>"$work/p$p.damage.log"
    if cmp -s "$work/p$p.before" "$work/p$p.damaged"; then fail "partition $p was not damaged"; fi
done
cat >"$work/restore.steps" <<EOF
send=mkdir -p /stick && mount -t exfat /dev/disk1p2 /stick && reflash /dev/disk0 $((boot_offset * 1048576)) 67108864 < /stick/saved/boot.img && echo BOOT""RESTORED || { echo STICK""FAIL: boot restore; poweroff -f; }
expect=BOOTRESTORED
send=reflash /dev/disk0 $((recovery_offset * 1048576)) 67108864 < /stick/saved/recovery.img && sync && umount /stick && echo RESTORE""DONE || { echo STICK""FAIL: recovery restore; poweroff -f; }
expect=RESTOREDONE
EOF
boot restore ide RESTOREDONE
for p in 1 2; do
    offset=$(cat "$work/p$p.offset")
    dd if="$work/target.img" of="$work/p$p.after" bs=1048576 skip="$offset" count=64 2>"$work/p$p.after.log"
    cmp "$work/p$p.before" "$work/p$p.after" || fail "partition $p restore differs"
    name=boot
    [ "$p" -eq 1 ] || name=recovery
    "$fixture" --check "$work/stick.img" "$start" --read "/saved/$name.img" \
        >"$work/p$p.saved" 2>"$work/p$p.saved.log"
    cmp "$work/p$p.before" "$work/p$p.saved" || fail "partition $p save differs"
done
"$fixture" --check "$work/stick.img" "$start" >"$work/final.check.log"
echo 'install-stick-test: result: pass: usb boot, root update, two 64 MiB saves and restore'
