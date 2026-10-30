#!/usr/bin/env sh

#
# build_acpica.sh: extract the core of ACPICA, the ACPI interpreter the kernel
# carries, from the pinned tarball into build/vendor/acpica/, where the kernel's
# build compiles it, and pack it as acpica.tar.bz2 there, which the images
# stage as /src/rach2/vendor/acpica.tar.bz2 for guest.mk to unpack.
#
# Nothing is compiled here. The kernel's Makefile and guest.mk compile the
# files, with flags of their own (rach2/sys/acpi/sources.mk). The tree holds
# the nine directories of source/components/ that the kernel builds and
# source/include/, flattened, so that a file is dispatcher/dsargs.c and the
# headers are include/, with the one patch applied. Every file is given the
# time of the extraction, not the tarball's, so that a tree extracted again
# after the patch changed is newer than every object compiled from the old
# one.
#
# The images take the archive and not the tree. os-real's RAM root is a
# module that ends below 32 MiB, where the DMA pool and i686's frame table
# are carved; the tree would add 4 MiB to it and the archive adds 0.45 MiB,
# which its 20 MiB hold. bzip2, which every host has, packs it smallest. The
# archive lists the files in name order, without the directories, the stamp
# or itself. The times and owners in it are this host's, which guest.mk does
# not restore (tar -xmo).
#
# The stamp, .stamp in the tree, holds the SHA-256 of the tarball, the patch
# and this script. When the tree's stamp already says so, the tree is left as
# it is and only the stamp's time moves, so a make that saw a newer tarball
# from a fresh checkout recompiles nothing. Two kernels built at once, such
# as the ordinary and the recovery kernel's sub-makes, may both run this: the
# lock, acpica.lock beside the tree, holds the second from the check of the
# stamp until the first has put its tree in place, so that neither removes a
# tree the other compiles, and the second then finds the stamp it wants. A
# lock whose holder has died, killed with its make, is taken over.
#
# The architecture is RACH2_ARCH's (mk/arch.sh); an x86-64 build extracts
# into build/x86_64/vendor/acpica/, where the stager's path map looks.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_acpica.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"
cd "$SCRIPT_DIR"

VERSION="20260930"
TARBALL="acpica-unix-${VERSION}.tar.gz"
PATCH="rach2-acenv.patch"
STAGE="$ARCH_BUILD/vendor/acpica"

# The core's directories under source/components/. The debugger and the
# disassembler are left out; resources/rsdump.c, which serves the debugger,
# is extracted but not compiled.
COMPONENTS="dispatcher events executer hardware namespace parser resources tables utilities"

# Pinned sha256 of the vendored tarball, as published with the release on
# github.com/open-acpica/acpica. The extraction refuses any other.
TARBALL_SHA256="aa18901b92e30749be0edc3081c8d550c61fce4fa37546fc6a65d367a4ae71a5"

sha256_of()
{
    make -s -C "$REPO" build/tools/verify/sha256 || return 1
    "$REPO/build/tools/verify/sha256" "$1" | awk '{print $1}'
}

for f in "$TARBALL" "$PATCH"; do
    if [ ! -f "$f" ]; then
        echo "acpica: $f missing (the vendored source)" >&2
        exit 1
    fi
done

GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "acpica: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi
WANT="$GOT $(sha256_of "$PATCH") $(sha256_of build_acpica.sh)"

# mkdir is the portable atomic test-and-set; the holder's pid is in the lock.
mkdir -p "$ARCH_BUILD/vendor"
LOCK="$ARCH_BUILD/vendor/acpica.lock"
WORK=
until mkdir "$LOCK" 2>/dev/null; do
    holder=$(cat "$LOCK/pid" 2>/dev/null) || holder=
    if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
        echo "acpica: taking over the lock of $holder, which has ended"
        rm -rf "$LOCK"
        continue
    fi
    sleep 1
done
trap 'rm -rf ${WORK:+"$WORK"} "$LOCK"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
echo $$ >"$LOCK/pid"

if [ -f "$STAGE/.stamp" ] && [ "$(cat "$STAGE/.stamp")" = "$WANT" ] &&
    [ -f "$STAGE/acpica.tar.bz2" ]; then
    touch "$STAGE/.stamp"
    exit 0
fi

echo "acpica: extracting $VERSION into $STAGE"
WORK="$(mktemp -d "$ARCH_BUILD/vendor/acpica.new.XXXXXX")"
tar xmzf "$TARBALL" -C "$WORK"
SRC="$WORK/acpica-unix-$VERSION/source"
mkdir "$WORK/acpica"
for d in $COMPONENTS; do
    mv "$SRC/components/$d" "$WORK/acpica/$d"
done
mv "$SRC/include" "$WORK/acpica/include"
mv "$WORK/acpica-unix-$VERSION/LICENSE.BSD-3-Clause" "$WORK/acpica/"
mv "$WORK/acpica-unix-$VERSION/LICENSE.GPL-2.0-only" "$WORK/acpica/"

if ! patch -s -p1 -F 0 --forward -d "$WORK/acpica" < "$PATCH"; then
    echo "acpica: $PATCH does not apply to ACPICA $VERSION; rebase it" >&2
    exit 1
fi
if ! grep -q '__RACH2__' "$WORK/acpica/include/platform/acenv.h"; then
    echo "acpica: the patch applied but acenv.h does not choose Rach2's header" >&2
    exit 1
fi
rm -f "$WORK/acpica/include/platform/acenv.h.orig"

(cd "$WORK/acpica" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$WORK/list"
case "$(tar --version 2>/dev/null)" in
    *bsdtar*) set -- --no-xattrs --no-mac-metadata ;;
    *) set -- ;;
esac
COPYFILE_DISABLE=1 tar -c -f "$WORK/acpica.tar" "$@" -C "$WORK/acpica" -T "$WORK/list"
bzip2 -9 "$WORK/acpica.tar"
mv "$WORK/acpica.tar.bz2" "$WORK/acpica/acpica.tar.bz2"
printf '%s\n' "$WANT" > "$WORK/acpica/.stamp"

rm -rf "$STAGE"
mv "$WORK/acpica" "$STAGE"
echo "acpica: extracted $VERSION ($(find "$STAGE" -name '*.c' | wc -l | tr -d ' ') C files)"
