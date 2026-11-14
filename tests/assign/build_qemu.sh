#!/bin/sh
# Build the assignment cases' outer emulator: QEMU 10.2.1 for x86-64 with
# qemu-vtd-cfi.patch, from the sources qemu.txt pins, into OUT.
#
# Usage: build_qemu.sh OUT
#
# OUT gets bin/qemu-system-x86_64, which finds the libraries of lib/ by its
# run path, $ORIGIN/../lib, and the firmware of share/qemu by its own place,
# as QEMU finds its data relative to its program; and `version`, which gives
# what the program prints for --version, its SHA-256 and each pinned source.
# Nothing is installed on the host: the build's libraries, headers, Ninja and
# pip come from the pinned Ubuntu packages, unpacked with dpkg-deb -x into
# the build's scratch directory, and the host gives only sh, Python 3 with
# its venv module, a C compiler, binutils, pkg-config, patch, xz and
# dpkg-deb. Each download is kept in build/cache/qemu and taken from
# RACH2_DOWNLOADS, or /var/cache/rach2-downloads where that exists, when the
# copy there has the pinned digest, and is otherwise downloaded and added
# there. RACH2_DOWNLOADS set empty turns the shared directory off. The root
# Makefile runs this script through `etude build`, so that the host's store
# keeps OUT for every copy.
set -eu

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd -P)
LIST=$REPO/tests/assign/qemu.txt
PATCH=$REPO/tests/assign/qemu-vtd-cfi.patch
CACHE=$REPO/build/cache/qemu
SHARED=${RACH2_DOWNLOADS-}
if [ -z "${RACH2_DOWNLOADS+set}" ] && [ -d /var/cache/rach2-downloads ]; then
    SHARED=/var/cache/rach2-downloads
fi
LIBDIR=usr/lib/x86_64-linux-gnu

die() {
    echo "assign-qemu: $*" >&2
    exit 1
}

[ $# -eq 1 ] || {
    echo 'usage: build_qemu.sh OUT' >&2
    exit 2
}
case $1 in
    /*) out=$1 ;;
    *) out=$(pwd -P)/$1 ;;
esac
for t in cc pkg-config python3 patch xz dpkg-deb strip readelf ldd; do
    command -v "$t" >/dev/null 2>&1 || die "$t is required"
done
python3 -c 'import venv' 2>/dev/null || die "Python's venv module is required"

work=
part=
trap 'rm -rf ${work:+"$work"} ${part:+"$part"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$CACHE" "$REPO/build/tmp"
work=$(mktemp -d "$REPO/build/tmp/assign-qemu.XXXXXX")

# digest FILE: print FILE's SHA-256.
digest() {
    sha256sum "$1" | awk '{print $1}'
}

# fetch NAME: set `fetched` to the path in CACHE of the source qemu.txt
# names NAME, checked against its digest, taken from SHARED or downloaded
# when CACHE lacks it. It runs in this shell, whose trap removes a part.
fetch() {
    line=$(awk -v n="$1" '$1 == n { print; exit }' "$LIST")
    [ -n "$line" ] || die "no pin for $1"
    # shellcheck disable=SC2086 # the list has four fields
    set -- $line
    f=$CACHE/${3##*/}
    s=${SHARED:+$SHARED/${3##*/}}
    if [ -f "$f" ]; then
        [ "$(digest "$f")" = "$4" ] || die "$f is not the pinned $1; remove it"
    elif [ -n "$s" ] && [ -f "$s" ] && [ "$(digest "$s")" = "$4" ]; then
        part=$(mktemp "$f.XXXXXX")
        cp "$s" "$part"
        mv "$part" "$f"
        part=
    else
        echo "assign-qemu: downloading $3" >&2
        part=$(mktemp "$f.XXXXXX")
        curl -sSfL --retry 3 -o "$part" "$3" || die "cannot download $3"
        got=$(digest "$part")
        [ "$got" = "$4" ] || die "refused: $1: sha256 $got, expected $4"
        mv "$part" "$f"
        part=
        if [ -n "$s" ] && [ ! -f "$s" ]; then
            # Another copy may add it meanwhile; failing to add it is no error.
            part=$(mktemp "$SHARED/.${3##*/}.XXXXXX") && cp "$f" "$part" &&
                chmod 664 "$part" && mv "$part" "$s" ||
                echo "assign-qemu: cannot add ${3##*/} to $SHARED" >&2
            rm -f "$part"
            part=
        fi
    fi
    fetched=$f
}

# The private packages, unpacked into one tree whose pkg-config files are
# moved from /usr to it. Their libraries are found by LIBRARY_PATH, not by a
# -L of their pkg-config files: Meson gives the program a run path to each
# directory a -L names, and its installation shortens that run path in place,
# which leaves the rest of the old one, this copy's path, in the program.
deps=$work/deps
mkdir -p "$deps"
awk '!/^#/ && NF == 4 && $1 != "qemu" { print $1 }' "$LIST" > "$work/packages"
while read -r name; do
    fetch "$name"
    dpkg-deb -x "$fetched" "$deps"
done < "$work/packages"
for pc in "$deps/$LIBDIR"/pkgconfig/*.pc "$deps"/usr/share/pkgconfig/*.pc; do
    [ -f "$pc" ] || continue
    # shellcheck disable=SC2016 # the variable is pkg-config's
    sed -i -e "s|^prefix=/usr\$|prefix=$deps/usr|" \
        -e '/^Libs/s|-L\${libdir} *||' "$pc"
done
PATH=$deps/usr/bin:$PATH
PYTHONPATH=$deps/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}
PKG_CONFIG_LIBDIR=$deps/$LIBDIR/pkgconfig:$deps/usr/share/pkgconfig
LIBRARY_PATH=$deps/$LIBDIR
LD_LIBRARY_PATH=$deps/$LIBDIR
export PATH PYTHONPATH PKG_CONFIG_LIBDIR LIBRARY_PATH LD_LIBRARY_PATH
unset PKG_CONFIG_PATH

fetch qemu
xz -dc "$fetched" | tar -xf - -C "$work"
patch -p1 -F 0 --batch --forward -d "$work/qemu-10.2.1" < "$PATCH" > /dev/null
mkdir "$work/build"
cd "$work/build"
# The run path is an old-style RPATH, which the dynamic linker searches for
# the libraries' own dependencies too, as it does not a RUNPATH. The file
# names that assertions keep are mapped from the scratch directory to /qemu,
# and the installed program is stripped of its debugging information, so that
# it names no path of the copy's.
# shellcheck disable=SC2016 # $ORIGIN is the dynamic linker's
"$work/qemu-10.2.1/configure" --target-list=x86_64-softmmu \
    --without-default-features --enable-system --enable-kvm --enable-tcg \
    --enable-slirp --enable-pixman --disable-docs --disable-guest-agent \
    --disable-rust --disable-download --with-pkgversion=rach2-vtd-cfi \
    --extra-cflags="-ffile-prefix-map=$work=/qemu" \
    --extra-ldflags='-Wl,--disable-new-dtags,-rpath,$ORIGIN/../lib' > configure.log 2>&1 || {
    tail -40 configure.log >&2
    die 'configure failed'
}
case " ${MAKEFLAGS-} " in
    *' -j'[0-9]*) jobs=$(printf '%s\n' " $MAKEFLAGS " | sed -n 's/.* -j\([0-9]*\) .*/\1/p') ;;
    *) jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4) ;;
esac
[ "${jobs:-0}" -ge 1 ] 2>/dev/null || jobs=4
[ "$jobs" -le 8 ] || jobs=8
ninja -j "$jobs" qemu-system-x86_64 > build.log 2>&1 || {
    tail -40 build.log >&2
    die 'the build failed'
}
DESTDIR=$work/install ninja install > install.log 2>&1 || {
    tail -40 install.log >&2
    die 'the installation failed'
}

# OUT: the program, the private libraries it loads and the data that QEMU's
# x86 machines load: SeaBIOS, the option ROMs of the network cards and the
# Linux and Multiboot loaders, the VGA BIOSes and the key maps. The other
# targets' firmware and EDK2's, near 300 MiB, stay out; a profile that boots
# by OVMF names its own image.
rm -rf "$out"
mkdir -p "$out/bin" "$out/lib" "$out/share/qemu"
installed=$work/install/usr/local
strip -o "$out/bin/qemu-system-x86_64" "$installed/bin/qemu-system-x86_64"
(
    cd "$installed/share/qemu"
    cp -R bios*.bin kvmvapic.bin linuxboot*.bin multiboot*.bin pvh.bin qboot.rom \
        vgabios*.bin efi-*.rom pxe-*.rom keymaps "$out/share/qemu/"
)
# Every library the program loads from a pinned package goes with it, its
# dependencies among them, whether or not the host has one of its own.
ldd "$out/bin/qemu-system-x86_64" > "$work/ldd" 2>&1 ||
    die "ldd cannot read the program: $(cat "$work/ldd")"
! grep -q 'not found' "$work/ldd" || die "the program needs a library no pinned package has"
awk -v d="$deps/" '$2 == "=>" && index($3, d) == 1 { print $3 }' "$work/ldd" > "$work/libs"
while read -r lib; do
    cp -L "$lib" "$out/lib/${lib##*/}"
done < "$work/libs"
rpath=$(readelf -d "$out/bin/qemu-system-x86_64" | sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]$/\1/p')
# shellcheck disable=SC2016 # $ORIGIN is the dynamic linker's
[ "$rpath" = '$ORIGIN/../lib' ] || die "the program's run path is $rpath, not \$ORIGIN/../lib"
! grep -rqF "$REPO" "$out" || die "OUT names the copy's path, which no other copy has"

# The record of what OUT holds, and the proof that it runs alone.
version=$(LD_LIBRARY_PATH='' "$out/bin/qemu-system-x86_64" --version | head -1) ||
    die 'the program does not run'
case $version in
    *rach2-vtd-cfi*) ;;
    *) die "the program is not the patched build: $version" ;;
esac
{
    printf '%s\n' "$version"
    printf 'sha256 %s\n' "$(digest "$out/bin/qemu-system-x86_64")"
    printf 'patch %s\n' "$(digest "$PATCH")"
    awk '!/^#/ && NF == 4 { print $1, $2, $4 }' "$LIST"
} > "$out/version"
echo "assign-qemu: result: built: $version"
