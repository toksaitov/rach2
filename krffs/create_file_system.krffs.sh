#!/usr/bin/env sh

#
# create_file_system.krffs.sh
#
# Creates an empty file system file of a specified size and initializes a KRFFS
# file system on it.
#
#     Usage:
#          ./create_file_system.krffs.sh [size in megabytes]
#
# Without the size parameter, the file size is set to 10 megabytes.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# The host tools build out of tree to build/krffs/ (mk/build.mk); create the scratch
# image there too, with the build/krffs mkfs, so nothing lands in the source tree.
BUILD="$(cd "$SCRIPT_DIR/.." && pwd)/build/krffs"
FILE_SYSTEM_FILE="$BUILD/file_system.krffs"
BLOCK_SIZE="1048576"
COUNT=${1:-10}

mkdir -p "$BUILD"
rm -f "$FILE_SYSTEM_FILE"
dd if=/dev/zero of="$FILE_SYSTEM_FILE" bs=$BLOCK_SIZE count=$COUNT
"$BUILD/mkfs.krffs" "$FILE_SYSTEM_FILE"
