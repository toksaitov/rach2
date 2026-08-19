#!/usr/bin/env sh

#
# mkgrubcore.sh: a GRUB core image with a marker inside it.
#
# `bootctl grub` writes the tail slot that is not in use and then points the
# entry sector at it. Proving that from outside needs the running core image to
# identify itself, because reading the pointer back proves only bytes at rest:
# a tool that wrote the pointer and nothing else would pass such a check while
# leaving the machine booting the old image.
#
# So this builds a core with a marker embedded in it. `grub-mkimage -c` puts a
# configuration inside the image, which runs before the one on the partition;
# an `echo` there reaches the serial console and names the build. The module
# list is the one tools/mkdisk_grub.sh uses, because a core image with
# different modules would be testing something else.
#
# Usage: mkgrubcore.sh <marker> <output>
#

set -e

MARKER="$1"
OUT="$2"

if [ -z "$MARKER" ] || [ -z "$OUT" ]; then
    echo "usage: mkgrubcore.sh <marker> <output>" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$(cd "$(dirname "$OUT")" && pwd)"
OUTBASE="$(basename "$OUT")"

if ! command -v docker >/dev/null 2>&1; then
    echo "mkgrubcore: docker required to build a core image (make doctor)" >&2
    exit 1
fi

docker run --rm --platform linux/amd64 -v "$REPO":"$REPO" -w "$REPO" \
    ubuntu:24.04 sh -e -c "
    apt-get update -qq && apt-get install -y -qq grub-pc-bin grub-common >/dev/null 2>&1
    # The serial terminal is set up here and not left to the configuration on
    # the partition, because this runs before that one: an echo before the
    # terminal exists reaches the screen and nothing a harness can read.
    {
        printf 'serial --unit=0 --speed=115200\n'
        printf 'terminal_output serial console\n'
        printf 'echo GRUBCORE=%s\n' '$MARKER'
    } > /tmp/embed.cfg
    grub-mkimage -O i386-pc -p '(hd0,msdos1)/boot/grub' -c /tmp/embed.cfg \
        -o '$OUTDIR/$OUTBASE' \
        biosdisk part_msdos fat normal multiboot configfile loadenv echo \
        test serial terminal
"

echo "mkgrubcore: wrote $OUT ($(wc -c < "$OUT") bytes, marker $MARKER)"
