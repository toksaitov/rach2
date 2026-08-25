#!/usr/bin/env sh

#
# build_libc.sh
#
# Builds musl libc for Rach2 from the pinned, vendored source tarball using the
# stock i686-elf-gcc cross compiler (no separate musl cross-toolchain is
# needed, since students already install i686-elf-gcc for the kernel). Produces a
# static libc.a plus the crt1/crti/crtn startup objects and the musl headers
# under install/, which Rach2 utilities link against (see Readme.md).
#
# Usage:
#     ./build_libc.sh
#
# Without arguments it verifies, extracts, configures, builds, and installs.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MUSL_VERSION="1.2.5"
TARBALL="musl-${MUSL_VERSION}.tar.gz"
SRC="musl-${MUSL_VERSION}"
PREFIX="$SCRIPT_DIR/install"

# Pinned sha256 of the vendored tarball (published on musl.libc.org). The build
# refuses to proceed if the tarball does not match; the version is locked.
SHA256="a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"

# The digest through the project tool (tools/verify/sha256.c), compiled on
# first use, so the build needs no coreutils or Perl digest command.
sha256_of()
{
    if [ ! -x "$SCRIPT_DIR/../../build/tools/verify/sha256" ]; then
        mkdir -p "$SCRIPT_DIR/../../build/tools/verify"
        cc -O2 -o "$SCRIPT_DIR/../../build/tools/verify/sha256" \
            "$SCRIPT_DIR/../../tools/verify/sha256.c" || return 1
    fi
    "$SCRIPT_DIR/../../build/tools/verify/sha256" "$1" | awk '{print $1}'
}

echo "libc: verifying the pinned musl tarball"
if [ ! -f "$TARBALL" ]; then
    echo "libc: $TARBALL not found; it is vendored beside this script" >&2
    exit 1
fi
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$SHA256" ]; then
    echo "libc: sha256 mismatch for $TARBALL" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi
echo "libc: verified ($SHA256)"

echo "libc: extracting"
rm -rf "$SRC"
tar xzf "$TARBALL"

# Apply the curated patches named in patches/series, in its order, before
# configuring. The vendored tree is otherwise pristine; this is the escape
# hatch for the rare fix that cannot be made any other way. The series file
# is the patch manifest the build stamp depends on: a named patch that is
# absent fails, and so does a patches/*.patch file the series does not name,
# so an added or removed patch always changes a tracked build input.
if [ -f patches/series ]; then
    while IFS= read -r p; do
        case "$p" in ''|'#'*) continue ;; esac
        if [ ! -f "patches/$p" ]; then
            echo "libc: patches/series names a missing patch: $p" >&2
            exit 1
        fi
        echo "libc: applying patches/$p"
        # --forward and no fuzz: a patch that no longer applies cleanly to a new
        # musl is a decision to make again, not a hunk to place approximately.
        if ! patch -p1 -F 0 --forward -d "$SRC" < "patches/$p"; then
            echo "libc: patches/$p does not apply to musl $MUSL_VERSION; rebase it" >&2
            exit 1
        fi
    done < patches/series
fi
for p in patches/*.patch; do
    [ -e "$p" ] || break
    if ! grep -qx "$(basename "$p")" patches/series 2>/dev/null; then
        echo "libc: $p is not named in patches/series; add it there or remove it" >&2
        exit 1
    fi
done

echo "libc: configuring (CROSS_COMPILE=i686-elf-, static, target i686)"
cd "$SRC"
# This is always the i686 cross build, so no caller's compiler choice may reach
# it. Two channels carry one in. musl's configure keeps an inherited CC and
# never tries the CROSS_COMPILE candidates (its trycc is a no-op once CC is
# set), and a `make CC=... ` above us records the override in MAKEFLAGS after
# a ` -- ` separator, which the musl make below would re-import even after CC
# itself is cleared. Clear CC and strip that override tail, but keep the rest
# of MAKEFLAGS: it carries the caller's jobserver, so this build shares the
# root `make -j` budget instead of choosing a job count of its own.
unset CC
unset MFLAGS
MAKEFLAGS="${MAKEFLAGS-}"; MAKEFLAGS="${MAKEFLAGS%% -- *}"
export MAKEFLAGS
CROSS_COMPILE=i686-elf- ./configure --target=i686 --disable-shared --prefix="$PREFIX"

echo "libc: building"
make

echo "libc: installing to $PREFIX (cleaned first for a reproducible tree)"
rm -rf "$PREFIX"
make install

echo "libc: built"
echo "  libc.a:  $PREFIX/lib/libc.a"
echo "  startup: $PREFIX/lib/crt1.o $PREFIX/lib/crti.o $PREFIX/lib/crtn.o"
echo "  headers: $PREFIX/include"
echo "libc: link a program against it (see Readme.md for the full recipe):"
echo "  i686-elf-gcc -static -nostdlib -nostartfiles -isystem $PREFIX/include \\"
echo "    -o prog crt1.o crti.o prog.c libc.a \$(i686-elf-gcc -print-libgcc-file-name) \\"
echo "    crtn.o -Wl,-z,noexecstack"
