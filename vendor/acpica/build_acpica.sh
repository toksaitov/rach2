#!/usr/bin/env sh

#
# build_acpica.sh: extract the core of ACPICA, the ACPI interpreter the kernel
# carries, from the pinned tarball into build/vendor/acpica/, where the kernel's
# build compiles it and the images stage it as /src/rach2/vendor/acpica.
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
# The stamp, .stamp in the tree, holds the SHA-256 of the tarball, the patch
# and this script. When the tree's stamp already says so, the tree is left as
# it is and only the stamp's time moves, so a make that saw a newer tarball
# from a fresh checkout recompiles nothing. Two kernels built at once may both
# run this on a missing tree: each extracts into a directory of its own and
# renames it into place, and the second, finding the first's tree with the
# same stamp, throws its own away.
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

if [ -f "$STAGE/.stamp" ] && [ "$(cat "$STAGE/.stamp")" = "$WANT" ]; then
    touch "$STAGE/.stamp"
    exit 0
fi

echo "acpica: extracting $VERSION into $STAGE"
mkdir -p "$ARCH_BUILD/vendor"
WORK="$(mktemp -d "$ARCH_BUILD/vendor/acpica.new.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
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
printf '%s\n' "$WANT" > "$WORK/acpica/.stamp"

if [ -f "$STAGE/.stamp" ] && [ "$(cat "$STAGE/.stamp")" = "$WANT" ]; then
    exit 0
fi
rm -rf "$STAGE"
mv "$WORK/acpica" "$STAGE"
echo "acpica: extracted $VERSION ($(find "$STAGE" -name '*.c' | wc -l | tr -d ' ') C files)"
