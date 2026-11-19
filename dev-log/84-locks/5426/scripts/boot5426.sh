#!/bin/sh
# Stage or restore 80-2's one-shot Debian boot on the Lenovo. Keep p1's
# images on the Lenovo. Run through the caller's machine identity check.
set -eu
umask 077

here=$(cd "$(dirname "$0")" && pwd)
state=$here/boot-state
part=/dev/bootpart

# Return the digest of an image or the live boot partition.
digest() {
    sum=$(sha256sum "$1") || return
    printf '%s\n' "${sum%% *}"
}

# Write the differing runs only and check the boot partition as a whole.
write_runs() {
    wanted=$1
    current=$2
    want=$(digest "$wanted")
    python3 -I "$here/p1runs.py" "$wanted" "$current" > "$state/runs"
    while read -r sector count; do
        dd if="$wanted" of="$part" bs=1M iflag=skip_bytes,count_bytes \
            oflag=seek_bytes skip="$((sector * 512))" seek="$((sector * 512))" \
            count="$((count * 512))" status=none
    done < "$state/runs"
    sync
    got=$(digest "$part")
    [ "$got" = "$want" ] || { echo 'boot5426: boot partition digest differs' >&2; exit 1; }
    echo "boot5426: boot partition sha256 $got"
}

grep -Fqw rach2.slot=candidate /proc/cmdline ||
    { echo 'boot5426: the release candidate must be running' >&2; exit 1; }
case ${1:-} in
    arm)
        mkdir "$state"
        dd if="$part" of="$state/before.img" bs=1M status=none
        before=$(digest "$state/before.img")
        printf '%s\n' "$before" > "$state/before.sha256"
        echo "boot5426: original boot partition sha256 $before"
        cp "$state/before.img" "$state/debian.img"
        dir=::/boot/grub/x86_64-efi
        "$here/mtools" -c mmd -i "$state/debian.img" "$dir" 2> /dev/null ||
            "$here/mtools" -c mdir -i "$state/debian.img" "$dir" > /dev/null
        "$here/mtools" -c mcopy -o -i "$state/debian.img" \
            "$here/linux.mod" "$here/ext2.mod" "$here/fshelp.mod" "$dir/"
        sync
        [ "$(digest "$part")" = "$before" ] ||
            { echo 'boot5426: boot partition changed before staging' >&2; exit 1; }
        write_runs "$state/debian.img" "$state/before.img"
        bootctl config "$here/boot.cfg"
        bootctl trial recovery
        bootctl status
        sync
        ;;
    restore)
        [ "$(digest "$state/before.img")" = "$(cat "$state/before.sha256")" ] ||
            { echo 'boot5426: saved boot partition digest differs' >&2; exit 1; }
        dd if="$part" of="$state/current.img" bs=1M status=none
        write_runs "$state/before.img" "$state/current.img"
        bootctl status
        rm -rf "$state"
        ;;
    *) echo 'usage: boot5426.sh arm|restore' >&2; exit 2 ;;
esac
