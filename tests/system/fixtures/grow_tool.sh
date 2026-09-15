#!/bin/sh
# In recovery RAM, wrap each real ext2 tool with independent counter reads.
name=${0##*/}
case "$name:$1" in
    grow-tool:whole) phase=whole ;;
    e2fsck:-fn) phase=check-readonly ;;
    e2fsck:-fp) phase=check-repair ;;
    resize2fs:*) phase=resize ;;
    *) exit 125 ;;
esac
snapshot()
{
    awk -v phase="$phase" -v edge="$1" '
        $1 == "disk-commands" { c = $2; seen++ }
        $1 == "disk-flushes" { f = $2; seen++ }
        END {
            if (seen != 2) exit 1
            printf "grow-witness: %s %s disk-commands %s disk-flushes %s\n",
                phase, edge, c, f
        }
    ' /proc/disk
}
if [ "$phase" = whole ]; then
    snapshot "$2"
    exit $?
fi
snapshot before || exit 125
rc=0
"/sbin/$name.actual" "$@" || rc=$?
snapshot after || exit 125
exit "$rc"
