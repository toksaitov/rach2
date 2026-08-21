#!/usr/bin/env sh

#
# grub_install.sh
#
# Copies Rach2 boot files into an existing GRUB directory without creating
# sparse files.
#
# Why: some GRUB builds crash on sparse files with errors like:
#   "error: sparse file not allowed."
#   "file.c:452: out of range pointer ..."
#
# Usage:
#   ./grub_install.sh /path/to/grub/boot
#
# Example (Linux, mounted USB at /mnt/usb):
#   make os-simple-image    # from the repo root: build the image to copy
#   cd rach2 && make        # build the kernel
#   ./grub_install.sh /mnt/usb/boot
#
# It copies the os-simple image by default; set RACH2_IMAGE=os-real for another.
# Then add a GRUB menu entry pointing at those files, e.g.:
#   menuentry "Rach2" {
#       multiboot /boot/kernel
#       module /boot/os-simple.krffs
#       boot
#   }
#

set -e

DEST_DIR="$1"
if [ -z "$DEST_DIR" ]; then
    echo "grub_install: usage: $0 /path/to/grub/boot" >&2
    exit 2
fi

copy_dense() {
    SRC="$1"
    DST="$2"

    if [ ! -f "$SRC" ]; then
        echo "grub_install: missing source file $SRC" >&2
        exit 1
    fi

    rm -f "$DST"
    cat "$SRC" > "$DST"
}

mkdir -p "$DEST_DIR"

echo "grub_install: copying kernel to $DEST_DIR/kernel"
copy_dense "../build/rach2/kernel" "$DEST_DIR/kernel"

IMAGE="${RACH2_IMAGE:-os-simple}"
if [ -f "../build/images/$IMAGE.krffs" ]; then
    echo "grub_install: copying RAM disk to $DEST_DIR/$IMAGE.krffs"
    copy_dense "../build/images/$IMAGE.krffs" "$DEST_DIR/$IMAGE.krffs"
else
    echo "grub_install: ../build/images/$IMAGE.krffs not found; build it (make $IMAGE-image) or set RACH2_IMAGE; skipping RAM disk copy"
fi

echo "grub_install: done"

