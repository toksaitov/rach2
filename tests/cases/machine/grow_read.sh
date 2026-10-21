#!/bin/sh
#
# grow_read.sh: read files off the exFAT store of the grow cases
# (tests/cases/machine.grow.json) on the host, as mkexfatfixture --read does,
# into files rather than a tool's standard output, which the scenario keeps
# only up to a bound below the size of the saved boot partition.
#
# Usage: grow_read.sh MKEXFATFIXTURE IMAGE PREFIX NAME...
#
# MKEXFATFIXTURE is the program, which may be a read-only file of the store;
# IMAGE is a disk whose volume starts at sector 2048. Each NAME, a path in the
# volume, is written to PREFIX followed by its last part. The status is 0, or
# 1 when a file cannot be read.
#
set -eu
[ "$#" -ge 4 ] || { echo "grow-read: usage: grow_read.sh MKEXFATFIXTURE IMAGE PREFIX NAME..." >&2; exit 2; }
program=$1
image=$2
prefix=$3
shift 3
dir=$(mktemp -d "${prefix%/*}/grow-read.XXXXXX")
trap 'rm -rf "$dir"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cp "$program" "$dir/mkexfatfixture"
chmod 755 "$dir/mkexfatfixture"
for name; do
    "$dir/mkexfatfixture" --check "$image" 2048 --read "$name" > "$prefix${name##*/}" || {
        echo "grow-read: $name cannot be read from $image" >&2
        exit 1
    }
done
