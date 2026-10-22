#!/bin/sh
# compile.sh: build a native Windows helper at its stable, suffixless goal.
set -eu
cc=$1
out=$2
shift 2
"$cc" -o "$out.exe" "$@"
mv -f "$out.exe" "$out"
