#!/bin/sh
# Compile the Wine probes, 64-bit and 32-bit, with MinGW in the Debian guest,
# and the X client xplace with Debian's gcc.
# The host sends the sources as a tar archive and receives the programs.
set -eu

if [ "${1:-}" = guest ]; then
    work=
    trap '[ -z "$work" ] || rm -rf "$work"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    work=$(mktemp -d /var/tmp/games-build.XXXXXX)
    mkdir -p "$work/src" "$work/stage/probes"
    tar -xf /tmp/games-input.tar -C "$work/src"
    sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install \
        gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 libx11-dev libxi-dev libxrandr-dev
    for target in x86_64:64 i686:32; do
        cc=${target%%:*}-w64-mingw32-gcc
        bits=${target#*:}
        for probe in gl:-lopengl32 d3d9:-ld3d9 hold: full:-ld3d9 tone:-lwinmm \
            dinput:-ldinput8,-ldxguid; do
            name=${probe%%:*}
            libs=$(echo "${probe#*:}" | tr , ' ')
            # shellcheck disable=SC2086 # the import libraries are words or none
            "$cc" -std=c99 -O2 -Wall -Wextra -Werror -o "$work/stage/probes/$name$bits.exe" \
                "$work/src/${name}probe.c" $libs -lgdi32 -luser32
        done
        "$cc" --version | head -n 1 >> "$work/stage/probes/compilers"
    done
    # The X client runs on the GNU root, whose libX11, libXi and libXrandr
    # are this guest's Debian 13 packages.
    gcc -std=c99 -O2 -Wall -Wextra -Werror -o "$work/stage/probes/xplace" "$work/src/xplace.c" \
        -lXrandr -lXi -lX11
    gcc --version | head -n 1 >> "$work/stage/probes/compilers"
    chmod -R a+rX "$work/stage"
    tar -cf /tmp/games-probes.tar -C "$work/stage" .
    echo 'games-build: result: compiled the probes for both architectures'
    exit 0
fi

out=$1
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$tree/build/tmp" "$(dirname "$out")"
work=$(mktemp -d "$tree/build/tmp/rach2-games-build.XXXXXX")
tar -cf "$work/games-input.tar" -C "$here" probe.h glprobe.c d3d9probe.c holdprobe.c fullprobe.c \
    toneprobe.c dinputprobe.c xplace.c
ref=$tree/tests/graphics/debian-ref/debian-ref.sh
sh "$ref" prepare -n
sh "$ref" run -c 4 -m 4096 -l -r "$out.record" -i "$work/games-input.tar" -i "$here/build.sh" \
    -o "$work/games-probes.tar" 'sh /tmp/build.sh guest'
mv "$work/games-probes.tar" "$out"
