#!/bin/sh
# guest.sh: the forward direction's guest, the Debian reference guest of the
# graphics tests as one image that QEMU on Rach2 boots.
#
#   guest.sh IMAGE KEY
#
# Prepares the reference guest when this tree's is not there yet
# (tests/graphics/debian-ref/debian-ref.sh prepare -n), then writes its
# prepared image, an overlay of Debian's cloud image, whole and without a
# backing file as the qcow2 image IMAGE, and the private key of its user gfx
# as KEY, readable by its owner alone. A machine case takes IMAGE as a disk's
# base, so that Rach2 sees the guest's disk as a device of 32 GiB. The
# prepared image and the key are in DEBIAN_REF_CACHE, by default
# build/cache/debian-ref of the clone whose worktree this is, as
# debian-ref.sh keeps them.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
common=$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$tree/.git")
cache=${DEBIAN_REF_CACHE:-$(dirname "$common")/build/cache/debian-ref}

die()
{
    echo "bed-forward: guest: $*" >&2
    exit 1
}

[ $# -eq 2 ] || {
    echo 'usage: guest.sh IMAGE KEY' >&2
    exit 2
}
image=$1 key=$2
trap 'rm -f "$image.tmp" "$key.tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
command -v qemu-img > /dev/null || die "no qemu-img"
sh "$tree/tests/graphics/debian-ref/debian-ref.sh" prepare -n >&2 || die "the reference guest was not prepared"
[ -f "$cache/prepared.qcow2" ] && [ -f "$cache/id_ed25519" ] || die "$cache has no prepared image with its key"
mkdir -p "$(dirname "$image")" "$(dirname "$key")"
qemu-img convert -O qcow2 "$cache/prepared.qcow2" "$image.tmp" || die "the image could not be written"
(umask 077 && cp "$cache/id_ed25519" "$key.tmp") || die "the key could not be copied"
mv "$key.tmp" "$key"
mv "$image.tmp" "$image"
echo "bed-forward: guest: $image, $(wc -c < "$image") bytes"
