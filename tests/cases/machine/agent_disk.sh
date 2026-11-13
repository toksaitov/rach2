#!/bin/sh
# agent_disk.sh: publish a partitioned sparse GNU disk for etude's cases.
set -eu
image=$1
disk=$2
mkmbr=$3
work=
trap '[ -z "$work" ] || rm -f "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
# The old disk goes first, so that it and its new copy never fill the disk
# together; a stopped run leaves no disk, which the next make builds again.
rm -f "$disk"
work=$(mktemp "$disk.XXXXXX")
size=$(($(wc -c < "$image") / 1048576))
dd if=/dev/zero of="$work" bs=1048576 count=0 seek=$((size + 2)) 2>/dev/null
dd if="$image" of="$work" bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null
"$mkmbr" --lba "$work" "2048:$((size * 2048)):83" >/dev/null
mv "$work" "$disk"
work=
