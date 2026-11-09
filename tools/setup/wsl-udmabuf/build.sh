#!/bin/sh
#
# build.sh: build udmabuf as a module of the running WSL kernel, out of the
# tree, from Microsoft's source of that release, for Venus over the host's
# lavapipe (tests/graphics/Readme.md). Runs as any account in WSL.
#
# Usage: sh tools/setup/wsl-udmabuf/build.sh
#
# The result is build/cache/wsl-udmabuf/RELEASE/udmabuf.ko of this tree, with
# the Module.symvers and the .config it was built against; load.sh loads it.
# The archive of the source, pinned in `sources`, stays in
# build/cache/wsl-udmabuf. JOBS sets make's jobs (the processors by default);
# KEEP=1 keeps the built tree in RELEASE/src, some 6 GiB.
#
# WSL's kernel is configured without CONFIG_UDMABUF, a bool that Kbuild
# cannot build as a module, so the driver is built from a copy of
# drivers/dma-buf/udmabuf.c with udmabuf-module.diff: it reads the memfd's
# seals itself, since memfd_fcntl() is not exported, and declares the GPL
# license and the DMA_BUF namespace that its imports need. The kernel checks
# each import's CRC (CONFIG_MODVERSIONS), and genksyms hashes the types as
# the compiler sees them, so the tree is configured from /proc/config.gz and
# given gcc 13.2's view by gcc13-view.diff: three Kconfig tests of the
# compiler answered as gcc 13.2 answers them, and no __typeof_unqual__, which
# gcc 14 has and which qualifies every per-CPU pointer. gcc 13.2, which built
# the running kernel, is not in Ubuntu 26.04; gcc-14 with that view gives
# every symbol the installed modules import the running kernel's CRC, which
# crc.sh checks for the whole tree and then for the module. The build is
# refused when the running kernel was built by another compiler, when the
# build's compiler is not gcc 14, or when the source's release is not the
# running kernel's.
set -eu

# The compiler that built the kernels gcc13-view.diff describes, as
# /proc/version names it.
KERNEL_CC='gcc (GCC) 13.2.0, GNU ld (GNU Binutils) 2.41'
CC=gcc-14
SUFFIX=-microsoft-standard-WSL2
URL=https://github.com/microsoft/WSL2-Linux-Kernel/archive/refs/tags

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)

die()
{
    echo "wsl-udmabuf: $*" >&2
    exit 1
}

release=$(uname -r)
version=${release%"$SUFFIX"}
[ "$version" != "$release" ] || die "$release is not a WSL kernel"
sha=$(awk -v v="$version" '$1 == v { print $2 }' "$here/sources")
[ -n "$sha" ] || die "no source of $version in $here/sources; add its archive's SHA-256 there"
case $(cat /proc/version) in
    *"($KERNEL_CC)"*) ;;
    *) die "the running kernel was not built by $KERNEL_CC; gcc13-view.diff gives that compiler's answers, so make it again for this one" ;;
esac
command -v "$CC" >/dev/null || die "no $CC; install the package gcc-14"
case $("$CC" -dumpfullversion) in
    14.*) ;;
    *) die "$CC is $("$CC" -dumpfullversion), not gcc 14" ;;
esac
for t in make flex bison bc perl patch pahole; do
    command -v "$t" >/dev/null || die "no $t"
done

cache=$tree/build/cache/wsl-udmabuf
out=$cache/$release
archive=$cache/linux-msft-wsl-$version.tar.gz
mkdir -p "$cache"
if [ ! -f "$archive" ]; then
    echo "wsl-udmabuf: fetching linux-msft-wsl-$version"
    curl -sSfL -o "$archive.part" "$URL/linux-msft-wsl-$version.tar.gz"
    mv "$archive.part" "$archive"
fi
echo "$sha  $archive" | sha256sum -c --quiet - || die "$archive: SHA-256 differs"

work=$out.work
finished='' status=0
rm -rf "$work"
mkdir -p "$work/src" "$work/mod"
trap 'status=$?; rm -rf "$work"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
echo "wsl-udmabuf: unpacking into $work"
tar -C "$work/src" --strip-components=1 -xzf "$archive"
cd "$work/src"
patch -p1 --forward --quiet < "$here/gcc13-view.diff" || die "gcc13-view.diff does not apply to $version"
patch -p1 --forward --quiet < "$here/udmabuf-module.diff" || die "udmabuf-module.diff does not apply to $version"
zcat /proc/config.gz > .config
make CC="$CC" HOSTCC="$CC" olddefconfig > "$work/config.log" 2>&1 || die "olddefconfig failed: $(tail -n 5 "$work/config.log")"
built=$(make -s CC="$CC" HOSTCC="$CC" kernelrelease)
[ "$built" = "$release" ] || die "the source is $built, not $release"

echo "wsl-udmabuf: building $release with $("$CC" --version | head -n 1)"
make CC="$CC" HOSTCC="$CC" -j"${JOBS:-$(nproc)}" vmlinux modules > "$work/build.log" 2>&1 ||
    die "the kernel's build failed: $(tail -n 20 "$work/build.log")"
cp drivers/dma-buf/udmabuf.c "$work/mod/"
echo 'obj-m := udmabuf.o' > "$work/mod/Kbuild"
make CC="$CC" HOSTCC="$CC" M="$work/mod" modules > "$work/mod.log" 2>&1 ||
    die "the module's build failed: $(tail -n 20 "$work/mod.log")"
! grep -q 'WARNING' "$work/mod.log" || die "the module's build warned: $(grep WARNING "$work/mod.log")"

sh "$here/crc.sh" -s Module.symvers
sh "$here/crc.sh" "$work/mod/udmabuf.ko"
vermagic=$(/usr/sbin/modinfo -F vermagic "$work/mod/udmabuf.ko")
case $vermagic in
    "$release "*) ;;
    *) die "the module's vermagic is $vermagic" ;;
esac

rm -rf "$out"
mkdir -p "$out"
cp "$work/mod/udmabuf.ko" Module.symvers "$out/"
cp .config "$out/config"
if [ "${KEEP:-0}" = 1 ]; then
    cd "$tree"
    mv "$work/src" "$out/src"
fi
echo "wsl-udmabuf: result: $out/udmabuf.ko, sha256 $(sha256sum "$out/udmabuf.ko" | cut -c1-64)"
finished=1
