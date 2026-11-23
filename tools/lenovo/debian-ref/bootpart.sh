#!/bin/sh
# Stage and restore the Debian boot while keeping both FAT copies on the Lenovo.
set -eu
state=/data/trial/debian-ref
mode=$1
stage=${2:-}
work=
loop=
mounted=
data_mounted=
finished=

cleanup()
{
    rc=$?
    trap - EXIT
    if [ -n "$mounted" ]; then umount "$work/fat" || exit 1; fi
    if [ -n "$loop" ]; then loopctl detach "$loop" || exit 1; fi
    if [ -n "$work" ]; then rm -rf "$work" || rc=1; fi
    if [ -n "$data_mounted" ]; then umount /data || rc=1; fi
    [ -n "$finished" ] || [ "$rc" -ne 0 ] || rc=1
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

hash()
{
    sum=$(sha256sum "$1")
    printf '%s\n' "${sum%% *}"
}

# p1write IMAGE HASH: write only the changed sector runs and verify the result.
p1write()
{
    dd if=/dev/bootpart of="$work/current.img" bs=1M status=none
    python3 -I "$state/p1runs.py" "$1" "$work/current.img" > "$work/runs"
    while read -r first count; do
        dd if="$1" of=/dev/bootpart bs=1M iflag=skip_bytes,count_bytes \
            oflag=seek_bytes skip="$((first * 512))" seek="$((first * 512))" \
            count="$((count * 512))" status=none
    done < "$work/runs"
    sync
    [ "$(hash /dev/bootpart)" = "$2" ]
    printf 'debian-boot: changed sector runs: %s\n' "$(wc -l < "$work/runs")"
}

restore()
{
    before=$(cat "$state/before.sha256")
    [ "$(hash "$state/before.img")" = "$before" ]
    p1write "$state/before.img" "$before"
    bootctl status
    printf 'debian-boot: restored: %s\n' "$before"
    rm -rf "$state"
    sync
}

arm()
{
    [ ! -e "$state" ] || { echo 'debian-boot: an earlier boot still needs restore' >&2; return 1; }
    [ -n "$stage" ]
    status=$(bootctl status)
    printf '%s\n' "$status" | grep -Fxq 'bootctl: saved_entry: candidate'
    printf '%s\n' "$status" | grep -Eq '^bootctl: next_entry: (absent|empty)$'
    printf '%s\n' "$status"
    dd if=/dev/bootpart of="$work/before.img" bs=1M status=none
    before=$(hash "$work/before.img")
    [ "$(hash /dev/bootpart)" = "$before" ]
    cp "$work/before.img" "$work/debian.img"
    mkdir "$work/fat"
    loopctl attach 3 "$work/debian.img"
    loop=3
    mount -t vfat /dev/loop3 "$work/fat"
    mounted=1
    modules=$work/fat/boot/grub/x86_64-efi
    mkdir -p "$modules"
    for module in linux ext2 fshelp; do
        cp "$stage/$module.mod" "$modules/$module.mod"
        cmp "$stage/$module.mod" "$modules/$module.mod"
    done
    umount "$work/fat"
    mounted=
    loopctl detach "$loop"
    loop=
    changed=$(hash "$work/debian.img")
    [ "$(hash /dev/bootpart)" = "$before" ]
    mkdir -m 700 "$state"
    mv "$work/before.img" "$state/before.img"
    cp "$stage/p1runs.py" "$state/p1runs.py"
    printf '%s\n' "$before" > "$state/before.sha256"
    sync
    printf 'debian-boot: before: %s\n' "$before"
    p1write "$work/debian.img" "$changed"
    printf 'debian-boot: edited: %s\n' "$changed"
    bootctl config "$stage/boot.cfg"
    bootctl trial recovery
    bootctl status
}

grep -Eq '(^| )rach2.slot=candidate( |$)' /proc/cmdline
[ -c /dev/bootpart ]
if grep -q ' /data ' /proc/mounts; then
    grep -q '^/dev/disk0p6 /data ext2 ' /proc/mounts
else
    mount -t ext2 /dev/disk0p6 /data
    data_mounted=1
fi
mkdir -p /data/trial
work=$(mktemp -d /data/trial/debian-ref.XXXXXX)
case "$mode" in
    arm) arm ;;
    restore) restore ;;
    *) exit 2 ;;
esac
finished=1
