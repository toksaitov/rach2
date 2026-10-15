#!/usr/bin/env sh

#
# check_diskpolicy.sh: read a kernel's disk policy off the built artifact.
#
# rach2/sys/block/diskcmd.c compiles one of two strings into the kernel:
# `rach2: disk policy: read-only` when it is built with -DRACH2_DISK_READONLY,
# as the metal kernel is, and `rach2: disk policy: read-write` otherwise.
# This script searches the binary for them with binfind and requires the
# expected string present and the other absent, so a kernel built with the
# other policy fails, and so does one that has both strings. mkiso.sh runs it
# before it builds an ISO around a kernel. The policy itself is described in
# rach2/sys/block/Readme.md.
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
make -s -C "$REPO" build/tools/inspect/binfind || exit 2

"$BINFIND" "rach2: disk policy: $POLICY" "$KERNEL" || {
    echo "check-diskpolicy: $KERNEL does not carry the $POLICY banner" >&2
    exit 1
}
if "$BINFIND" "rach2: disk policy: $OTHER" "$KERNEL"; then
    echo "check-diskpolicy: $KERNEL carries both policy banners" >&2
    exit 1
fi
echo "check-diskpolicy: ok: $KERNEL is $POLICY"
