#!/usr/bin/env sh

#
# check_recovery.sh: read a kernel's disk authority off the built artifact.
#
# rach2/sys/block/recovery.c compiles one of two strings into the kernel:
# `rach2: disk authority: recovery` when it is built with -DRACH2_RECOVERY,
# whose disk nodes can be written, and `rach2: disk authority: ordinary`
# otherwise. This script searches the binary for them with binfind and
# requires the expected string present and the other absent, so a kernel
# built into the wrong tree fails before anything stages it.
#
# The string and the write handler are separate conditionals, so the check
# finds a build mistake and says nothing of a kernel edited afterwards.
# `tests/run authority` runs it on both kernels, then boots them and requires
# a write by the ordinary one to fail. The authority itself is described in
# rach2/sys/block/Readme.md.
#
# Usage: check_recovery.sh <kernel> recovery|ordinary
#

set -eu

KERNEL="${1:-}"
AUTH="${2:-}"

case "$AUTH" in
    recovery) OTHER="ordinary" ;;
    ordinary) OTHER="recovery" ;;
    *)
        echo "usage: check_recovery.sh <kernel> recovery|ordinary" >&2
        exit 2
        ;;
esac
[ -s "$KERNEL" ] || {
    echo "check-recovery: $KERNEL is missing or empty, so this check proved nothing" >&2
    exit 1
}

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BINFIND="$REPO/build/tools/inspect/binfind"
make -s -C "$REPO" build/tools/inspect/binfind || exit 2

"$BINFIND" "rach2: disk authority: $AUTH" "$KERNEL" || {
    echo "check-recovery: $KERNEL does not carry the $AUTH banner" >&2
    exit 1
}
if "$BINFIND" "rach2: disk authority: $OTHER" "$KERNEL"; then
    echo "check-recovery: $KERNEL carries both authority banners" >&2
    exit 1
fi
echo "check-recovery: ok: $KERNEL has $AUTH authority"
