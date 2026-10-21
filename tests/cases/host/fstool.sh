#!/bin/sh
#
# fstool.sh: the host's own exFAT and FAT tools, for the machine cases that
# judge a volume the kernel wrote with a formatter or a checker that shares
# nothing with it: macOS's newfs_exfat and fsck_exfat, exfatprogs and
# dosfstools on Linux, and the same programs in etude's tooling machine
# (tools/etude/Readme.md, The tooling machine) on a host that has neither.
#
# `exfat-mkfs IMAGE PART START COUNT LABEL` makes an exFAT volume labelled
# LABEL in MBR partition PART of the disk image IMAGE, which starts at sector
# START and has COUNT sectors. The partition must read as zeros, as an empty
# disk does, since the volume is copied in sparse and its zero blocks are not
# written. `exfat-fsck IMAGE START COUNT [repair]` checks a copy of the exFAT
# volume at sectors START to START + COUNT of IMAGE, without changing it; with
# `repair` the checker repairs the copy first and must then call it clean.
# `fat-fsck IMAGE START COUNT` checks a copy of a FAT volume with
# `fsck.fat -n`. IMAGE itself changes only under exfat-mkfs.
#
# The first line of output is `fstool: tool: NAME`, the program that ran and
# where, or `fstool: absent: WHAT` when the host has no such program and no
# made tooling machine. The exit status is 0 when the volume was made or is
# clean, 1 when the tool failed or found a fault, 2 for a wrong usage, and 3
# when no tool was found. The copies are made in the current directory, the
# case's scratch directory, and removed on every exit.
#
# Usage:
#     sh tests/cases/host/fstool.sh exfat-mkfs IMAGE PART START COUNT LABEL
#     sh tests/cases/host/fstool.sh exfat-fsck IMAGE START COUNT [repair]
#     sh tests/cases/host/fstool.sh fat-fsck IMAGE START COUNT
#

set -u

usage() {
    echo "fstool: usage: fstool.sh exfat-mkfs IMAGE PART START COUNT LABEL" >&2
    echo "       fstool.sh exfat-fsck IMAGE START COUNT [repair]" >&2
    echo "       fstool.sh fat-fsck IMAGE START COUNT" >&2
    exit 2
}

# number WORD: succeed when WORD is a decimal count.
number() {
    case "$1" in '' | *[!0-9]*) return 1 ;; esac
}

COPY=
DEV=
JOB=
# shellcheck disable=SC2329 # called from the exit trap
cleanup() {
    [ -z "$DEV" ] || hdiutil detach "$DEV" > /dev/null 2>&1
    [ -z "$COPY" ] || rm -f "$COPY"
    [ -z "$JOB" ] || rm -f "$JOB"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ "$#" -ge 1 ] || usage
VERB=$1
shift
case "$VERB" in
    exfat-mkfs)
        [ "$#" -eq 5 ] || usage
        IMAGE=$1 PART=$2 START=$3 COUNT=$4 LABEL=$5
        number "$PART" || usage ;;
    exfat-fsck)
        [ "$#" -eq 3 ] || [ "$#" -eq 4 ] || usage
        IMAGE=$1 START=$2 COUNT=$3 REPAIR=${4-}
        case "$REPAIR" in '' | repair) ;; *) usage ;; esac ;;
    fat-fsck)
        [ "$#" -eq 3 ] || usage
        IMAGE=$1 START=$2 COUNT=$3 ;;
    *) usage ;;
esac
{ number "$START" && number "$COUNT"; } || usage
[ -f "$IMAGE" ] || { echo "fstool: no image $IMAGE" >&2; exit 2; }

# The tooling machine, as tests/system/tooling.sh runs it for the old suites.
REPO=${ETUDE_TREE:-.}
TOOLING=
if [ -f "$REPO/tests/system/tooling.sh" ]; then
    # shellcheck source=tests/system/tooling.sh
    . "$REPO/tests/system/tooling.sh"
    tooling_made && TOOLING=1
fi

# Whole mebibytes move a mebibyte at a time, and anything else a sector.
if [ $((START % 2048)) -eq 0 ] && [ $((COUNT % 2048)) -eq 0 ]; then
    BS=1048576 SKIP=$((START / 2048)) N=$((COUNT / 2048))
else
    BS=512 SKIP=$START N=$COUNT
fi

# copy_out: copy the volume out of IMAGE into COPY, sparse where the host's
# dd can make it so.
copy_out() {
    COPY=$(pwd)/fstool.$$.vol
    rm -f "$COPY"
    dd if="$IMAGE" of="$COPY" bs=$BS skip="$SKIP" count="$N" conv=sparse 2> /dev/null ||
        dd if="$IMAGE" of="$COPY" bs=$BS skip="$SKIP" count="$N" 2> /dev/null ||
        { echo "fstool: the volume cannot be copied out of $IMAGE" >&2; exit 1; }
}

# attach FILE: attach FILE as a raw disk on macOS, putting its node in DEV.
attach() {
    DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$1" 2> /dev/null |
        awk 'NR == 1 {print $1}')
    [ -n "$DEV" ] || { echo "fstool: hdiutil cannot attach $1" >&2; exit 1; }
}

# job LINE...: write the tooling machine's job, one LINE a line, to JOB.
job() {
    JOB=$(pwd)/fstool.$$.job
    printf '%s\n' "$@" > "$JOB"
}

# check FSCK: check COPY with FSCK, a checker that takes -n and -y and the
# file or node, repairing it first when REPAIR is set; give its status.
check() {
    target=$COPY
    [ -z "$DEV" ] || target=$DEV
    if [ -n "${REPAIR:-}" ]; then
        "$1" -y "$target"
        echo "fstool: the repair ended with status $?"
    fi
    "$1" -n "$target"
}

case "$VERB" in
    exfat-mkfs)
        if command -v hdiutil > /dev/null 2>&1 && command -v newfs_exfat > /dev/null 2>&1; then
            echo "fstool: tool: newfs_exfat"
            attach "$IMAGE"
            newfs_exfat -v "$LABEL" "${DEV}s$PART" || exit 1
        elif command -v mkfs.exfat > /dev/null 2>&1; then
            echo "fstool: tool: mkfs.exfat"
            COPY=$(pwd)/fstool.$$.vol
            rm -f "$COPY"
            dd if=/dev/zero of="$COPY" bs=512 count=0 seek="$COUNT" 2> /dev/null &&
                mkfs.exfat -L "$LABEL" "$COPY" || exit 1
            dd if="$COPY" of="$IMAGE" bs=$BS seek="$SKIP" conv=notrunc,sparse 2> /dev/null ||
                { echo "fstool: the volume cannot be written into $IMAGE" >&2; exit 1; }
        elif [ -n "$TOOLING" ]; then
            echo "fstool: tool: mkfs.exfat in the tooling machine"
            job "mkfs.exfat -L '$LABEL' \"\$ETUDE_DISK1-part$PART\""
            tooling run -d "$IMAGE" "$JOB" || exit 1
        else
            echo "fstool: absent: newfs_exfat, mkfs.exfat and a made tooling machine"
            exit 3
        fi ;;
    exfat-fsck)
        if command -v hdiutil > /dev/null 2>&1 && command -v fsck_exfat > /dev/null 2>&1; then
            echo "fstool: tool: fsck_exfat"
            copy_out
            attach "$COPY"
            check fsck_exfat || exit 1
        elif command -v fsck.exfat > /dev/null 2>&1; then
            echo "fstool: tool: fsck.exfat"
            copy_out
            check fsck.exfat || exit 1
        elif [ -n "$TOOLING" ]; then
            echo "fstool: tool: fsck.exfat in the tooling machine"
            copy_out
            # shellcheck disable=SC2016 # the job's lines expand in the machine
            if [ -n "$REPAIR" ]; then
                job 'fsck.exfat -y "$ETUDE_DISK1"; echo "fstool: the repair ended with status $?"' \
                    'fsck.exfat -n "$ETUDE_DISK1"'
            else
                job 'fsck.exfat -n "$ETUDE_DISK1"'
            fi
            tooling run -d "$COPY" "$JOB" || exit 1
        else
            echo "fstool: absent: fsck_exfat, fsck.exfat and a made tooling machine"
            exit 3
        fi ;;
    fat-fsck)
        if command -v fsck.fat > /dev/null 2>&1; then
            echo "fstool: tool: fsck.fat"
            copy_out
            fsck.fat -n "$COPY" || exit 1
        elif [ -n "$TOOLING" ]; then
            echo "fstool: tool: fsck.fat in the tooling machine"
            copy_out
            # shellcheck disable=SC2016 # the job's line expands in the machine
            job 'fsck.fat -n "$ETUDE_DISK1"'
            tooling run -d "$COPY" "$JOB" || exit 1
        else
            echo "fstool: absent: fsck.fat and a made tooling machine"
            exit 3
        fi ;;
esac
exit 0
