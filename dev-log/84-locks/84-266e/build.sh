#!/bin/sh
# build.sh: build dredshim's libd3d12.so into OUT, against a checkout of
# Microsoft's DirectX-Headers (84-266e used 61c7718954ec of 2026-11-09).
# Usage: sh build.sh HEADERS OUT
set -eu
here=$(cd "$(dirname "$0")" && pwd)
hdrs=$1
out=$2
mkdir -p "$out"
grep -oE 'D3D12_AUTO_BREADCRUMB_OP_[A-Z0-9_]+[[:space:]]*=' \
    "$hdrs/include/directx/d3d12.h" | sed 's/[[:space:]]*=//' | sort -u |
    awk '{ n = $1; sub("D3D12_AUTO_BREADCRUMB_OP_", "", n)
           printf "\tcase %s: return \"%s\";\n", $1, n }' > "$out/ops.inc"
gcc -O1 -g -Wall -fPIC -shared -fvisibility=hidden -I"$hdrs/include" \
    -I"$hdrs/include/wsl/stubs" -I"$out" -o "$out/libd3d12.so" \
    "$here/dredshim.c" "$here/fwd.c" -ldl -lpthread
