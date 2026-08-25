#!/usr/bin/env sh

#
# check_recovery.sh: read a kernel's disk authority off the built artifact.
#
# The recovery kernel may write any sector of the disk and the ordinary one
# may not (see rach2/sys/block/recovery.c). Each build carries an explicit
# banner naming which it is; this reads the banner out of the binary through
# the binfind byte searcher and requires the expected one present and the
# other absent, so a kernel built into the wrong tree fails before anything
# stages it. This check catches a build mistake, not a tampered kernel: the
# banner is a string in .rodata and the write handler is a separate
# conditional. The behavioral proof is `tests/run authority`, which boots
# both kernels and requires the ordinary one to refuse a write; this check
# exists so a build mistake is caught before that test runs, not instead of
# it.
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
if [ ! -x "$BINFIND" ]; then
    mkdir -p "${BINFIND%/*}"
    cc -O2 -o "$BINFIND" "$REPO/tools/inspect/binfind.c" || exit 2
fi

"$BINFIND" "rach2: disk authority: $AUTH" "$KERNEL" || {
    echo "check-recovery: $KERNEL does not carry the $AUTH banner" >&2
    exit 1
}
if "$BINFIND" "rach2: disk authority: $OTHER" "$KERNEL"; then
    echo "check-recovery: $KERNEL carries both authority banners" >&2
    exit 1
fi
echo "check-recovery: ok: $KERNEL has $AUTH authority"
