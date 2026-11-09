#!/bin/sh
# Put the reference guest's Xvfb and non-libc libraries on the test disk.
set -eu
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
if [ "${1:-}" = guest ]; then
    work=$(mktemp -d /var/tmp/d3d-runtime.XXXXXX)
    mkdir -p "$work/xvfb/lib"
    cp /usr/bin/Xvfb "$work/xvfb/"
    ldd /usr/bin/Xvfb | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' > "$work/needed"
    while read -r file; do
        case ${file##*/} in
            libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*) continue ;;
        esac
        cp -L "$file" "$work/xvfb/lib/"
    done < "$work/needed"
    tar -cf /tmp/d3d-runtime.tar -C "$work" xvfb
    exit 0
fi
out=$1
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
mkdir -p "$tree/build/tmp" "$(dirname "$out")"
work=$(mktemp -d "$tree/build/tmp/d3d-runtime.XXXXXX")
sh "$here/../debian-ref/debian-ref.sh" prepare -n
sh "$here/../debian-ref/debian-ref.sh" run -c 2 -m 2048 \
    -i "$here/runtime.sh" -o "$work/d3d-runtime.tar" 'sh /tmp/runtime.sh guest'
mv "$work/d3d-runtime.tar" "$out"
