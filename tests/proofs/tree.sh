#!/bin/sh
# tree.sh: lay out the private tree of a mutation proof's build: copies of
# the tree's PATHs under DIR, which is made afresh, with the proof's private
# sources, the files under SRC, copied over them. A test compiled there finds
# the private sources wherever it looks for them in the tree, relative to its
# own file or through -I, and the tree's own sources are never read.
#
# Usage: sh tests/proofs/tree.sh DIR SRC PATH...
set -eu
[ "$#" -ge 3 ] || { echo 'usage: tree.sh DIR SRC PATH...' >&2; exit 2; }
dir=$1
src=$2
shift 2
rm -rf "$dir"
mkdir -p "$dir"
for path do
    mkdir -p "$dir/$(dirname -- "$path")"
    cp -R "$path" "$dir/$path"
done
cp -R "$src/." "$dir/"
