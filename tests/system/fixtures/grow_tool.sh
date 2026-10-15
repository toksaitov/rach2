#!/bin/sh
# grow_tool.sh: a wrapper the grow suite installs over e2fsck and resize2fs
# in the recovery machine's RAM root, which prints the disk's counters from
# /proc/disk before and after each run of the tool it replaces.
#
# Installed as /sbin/e2fsck, for -fn or -fp, or as /sbin/resize2fs, it runs
# the tool moved to NAME.actual and exits with its status, or with 125 when a
# counter read fails or the command line is another. `grow-tool whole EDGE`
# prints the counters for the start or the end of the whole grow. Each read
# is one `grow-witness:` line, which checkers/grow_measure.awk compares with
# growroot's own samples.
name=${0##*/}
case "$name:$1" in
    grow-tool:whole) phase=whole ;;
    e2fsck:-fn) phase=check-readonly ;;
    e2fsck:-fp) phase=check-repair ;;
    resize2fs:*) phase=resize ;;
    *) exit 125 ;;
esac
# Print a `grow-witness:` line for the phase and edge $1, from the counters
# in /proc/disk; fail when it lacks either.
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
