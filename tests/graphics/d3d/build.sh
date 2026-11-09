#!/bin/sh
# Build the Direct3D probes and upstream suites in the pinned Debian guest.
# The host publishes a tar archive; the guest performs compilation alone.
set -eu

if [ "${1:-}" = guest ]; then
    work=/var/tmp/d3d-build
    trap 'rm -rf "$work"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    mkdir -p "$work/stage/probes" "$work/stage/wine" "$work/stage/vkd3d"
    cd "$work"
    tar -xf /tmp/d3d-input.tar
    sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install \
        gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 mingw-w64-tools \
        flex bison libvulkan-dev glslang-tools meson ninja-build cmake pkg-config
    for n in 9 11 12; do
        libs=-ld3d9
        [ "$n" = 9 ] || libs="-ld3d$n -ldxgi -ld3dcompiler_47"
        # shellcheck disable=SC2086 # the selected import libraries are words
        x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -Werror \
            "src/d3d${n}probe.c" -o "stage/probes/d3d$n.exe" $libs
    done
    tar -xf sources/wine-11.0.tar.xz
    mkdir wine-build
    cd wine-build
    ../wine-11.0/configure --enable-win64 --without-x --without-freetype
    for e in d3d9 d3d10 d3d10_1 d3d10core d3d11 d3d12 dxgi; do
        make -s -j"$(nproc)" "dlls/$e/tests/x86_64-windows/${e}_test.exe"
        cp "dlls/$e/tests/x86_64-windows/${e}_test.exe" "../stage/wine/"
    done
    cd ..
    unpack()
    {
        mkdir -p "$2"
        tar -xf "sources/$1.tar.gz" -C "$2" --strip-components=1
    }
    unpack 8293130ebe370b1deb761898ae0d397d680837de vkd3d
    unpack 8b48f0e0392d76be4fdcea2a3ba3ea99dddaae5b vkd3d/subprojects/dxil-spirv
    unpack c63848ecf2200425511319fd8bf2c17b751e501e vkd3d/subprojects/dxil-spirv/third_party/spirv-headers
    unpack 8dae2d8573af08378c6059aca3d5987da99d20b5 vkd3d/subprojects/dxil-spirv/subprojects/dxbc-spirv
    unpack c8ad050fcb29e42a2f57d9f59e97488f465c436d vkd3d/subprojects/dxil-spirv/subprojects/dxbc-spirv/submodules/spirv_headers
    unpack f88a2d766840fc825af1fc065977953ba1fa4a91 vkd3d/khronos/SPIRV-Headers
    unpack ee2ec5fd83dafce291024683b50dc89219333076 vkd3d/khronos/Vulkan-Headers
    mkdir bin
    ln -s /usr/bin/x86_64-w64-mingw32-widl bin/widl
    PATH="$work/bin:$PATH"
    export PATH
    meson setup vkd3d-build vkd3d -Denable_tests=true --buildtype=release \
        --strip --prefix="$work/vkd3d-install" --bindir=x64 --libdir=x64
    ninja -C vkd3d-build -j"$(nproc)"
    cp vkd3d-build/tests/d3d12 stage/vkd3d/
    find vkd3d-build -type f -name '*.so*' -exec cp {} stage/vkd3d/ \;
    chmod -R a+rX stage
    tar -cf /tmp/d3d-bundle.tar -C stage .
    echo 'd3d-build: result: compiled the probes and both suites'
    exit 0
fi

out=$1
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../../.." && pwd)
GNU=$REPO/vendor/gnu
# shellcheck source=../../../vendor/gnu/debian/fetch_asset.sh
. "$GNU/debian/fetch_asset.sh"
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$REPO/build/tmp" "$(dirname "$out")"
work=$(mktemp -d "$REPO/build/tmp/d3d-build.XXXXXX")
mkdir -p "$work/sources" "$work/src"
# shellcheck disable=SC2094 # asset reads the manifest and writes only the cache
while read -r name _version url _digest; do
    case "$name" in ''|'#'*) continue ;; esac
    file=$(asset "$name" "$here/sources.txt")
    cp "$file" "$work/sources/${url##*/}"
done < "$here/sources.txt"
cp "$here"/*.c "$here/probe.h" "$work/src/"
tar -cf "$work/d3d-input.tar" -C "$work" sources src
ref=$REPO/tests/graphics/debian-ref/debian-ref.sh
sh "$ref" prepare -n
sh "$ref" run -c 8 -m 16384 -r "$out.record" \
    -i "$work/d3d-input.tar" -i "$here/build.sh" -o "$work/d3d-bundle.tar" \
    'sh /tmp/build.sh guest'
mv "$work/d3d-bundle.tar" "$out"
