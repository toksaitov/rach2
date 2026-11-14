#!/bin/sh
# Compile the GNU helper's products and fixture controllers during preparation.
# No test runs here. The completed image supplies every executable at runtime.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
REPO=$repo
. "$repo/vendor/gnu/arch.sh"
out=$GNU_OUT/host-products
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" "$repo/build/tests"
work=$(mktemp -d "$repo/build/tests/host-products.XXXXXX")
cd "$repo"
compiler=$GNU_OUT/cross/bin/$GNU_TARGET-gcc
make --no-print-directory -f tests/cases/host/products.mk \
    CC="$compiler --sysroot=$GNU_OUT/root" PREFIX="$out" \
    SANITIZER_CC="$compiler --sysroot=$GNU_OUT/root -B$GNU_OUT/sanitizers/compiler/ -L$GNU_OUT/sanitizers/root/usr/lib"
for tool in mke2fs debugfs e2fsck resize2fs dumpe2fs tune2fs; do
    cp "$ARCH_BUILD/vendor/e2fsprogs/$tool" "$out/$tool"
done
cp "$ARCH_BUILD/vendor/mtools/mtools" "$out/mtools"
for tool in mcopy mdir minfo mtype mformat mmd mdel mdeltree; do
    ln -sf mtools "$out/$tool"
done

# Build both TinyCC assemblers for this helper's execution architecture.
# The tarball and seven patches are the same as the installed compiler's.
tarball=$repo/vendor/tcc/tcc-0.9.27.tar.bz2
expected=$(sed -n 's/^TARBALL_SHA256="\([0-9a-f]*\)"$/\1/p' vendor/tcc/build_tcc.sh)
actual=$(build/tools/verify/sha256 "$tarball")
actual=${actual%% *}
[ "$actual" = "$expected" ] || { echo 'host-products: TinyCC digest differs' >&2; exit 1; }
tar -xf "$tarball" -C "$work"
src=$work/tcc-0.9.27
cd "$src"
for name in rach2-output-policy.patch rach2-x86_64-static-link.patch \
    rach2-x86-asm.patch rach2-linker-script.patch \
    rach2-builtin-offsetof.patch rach2-va-list-trap.patch rach2-x86_64-zero-extend.patch; do
    patch -p1 --batch --forward < "$repo/vendor/tcc/$name"
done
case "$RACH2_ARCH" in i686) cpu=i386 ;; *) cpu=x86_64 ;; esac
./configure --cross-prefix="$GNU_OUT/cross/bin/$GNU_TARGET-" --cpu="$cpu" \
    --extra-cflags="--sysroot=$GNU_OUT/root" --extra-ldflags="--sysroot=$GNU_OUT/root"
make i386-tcc x86_64-tcc
mkdir -p "$out/tcc"
cp i386-tcc x86_64-tcc "$out/tcc/"
