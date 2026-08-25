#!/usr/bin/env sh

#
# check_diskpolicy.sh: read a kernel's disk policy off the built artifact.
#
# Milestone 24 put Rach2 into a laptop whose SSD has somebody's Windows, and
# nothing the ordinary kernel does may write that disk. The rule is enforced
# by a build switch (`-DRACH2_DISK_READONLY`, see rach2/sys/block/diskcmd.c),
# and a build switch is only as good as the check that the artifact actually
# got built with it. diskcmd.c has an explicit banner naming the policy, the
# kernel prints it at boot, and this reads it out of the binary through the
# binfind byte searcher and requires BOTH halves: the expected banner
# present, and the other one absent. A kernel built without the switch fails
# this, which is the case worth catching, and so does one that somehow has
# both.
#
# Usage: check_diskpolicy.sh <kernel> read-only|read-write
#

set -eu

KERNEL="${1:-}"
POLICY="${2:-}"

case "$POLICY" in
    read-only)  OTHER="read-write" ;;
    read-write) OTHER="read-only" ;;
    *)
        echo "usage: check_diskpolicy.sh <kernel> read-only|read-write" >&2
        exit 2
        ;;
esac
[ -s "$KERNEL" ] || {
    echo "check-diskpolicy: $KERNEL is missing or empty, so this check proved nothing" >&2
    exit 1
}

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BINFIND="$REPO/build/tools/inspect/binfind"
if [ ! -x "$BINFIND" ]; then
    mkdir -p "${BINFIND%/*}"
    cc -O2 -o "$BINFIND" "$REPO/tools/inspect/binfind.c" || exit 2
fi

"$BINFIND" "rach2: disk policy: $POLICY" "$KERNEL" || {
    echo "check-diskpolicy: $KERNEL does not carry the $POLICY banner" >&2
    exit 1
}
if "$BINFIND" "rach2: disk policy: $OTHER" "$KERNEL"; then
    echo "check-diskpolicy: $KERNEL carries both policy banners" >&2
    exit 1
fi
echo "check-diskpolicy: ok: $KERNEL is $POLICY"
