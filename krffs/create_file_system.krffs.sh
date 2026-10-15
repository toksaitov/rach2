#!/usr/bin/env sh

#
# create_file_system.krffs.sh: make build/krffs/file_system.krffs, an empty
# KRFFS of SIZE_MIB mebibytes, 10 by default, for the FUSE driver to mount.
#
# Usage: create_file_system.krffs.sh [SIZE_MIB]
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# The image is made beside the host tools, with their mkfs.krffs.
BUILD="$(cd "$SCRIPT_DIR/.." && pwd)/build/krffs"
FILE_SYSTEM_FILE="$BUILD/file_system.krffs"
BLOCK_SIZE="1048576"
COUNT=${1:-10}

mkdir -p "$BUILD"
rm -f "$FILE_SYSTEM_FILE"
dd if=/dev/zero of="$FILE_SYSTEM_FILE" bs=$BLOCK_SIZE count=$COUNT
"$BUILD/mkfs.krffs" "$FILE_SYSTEM_FILE"
