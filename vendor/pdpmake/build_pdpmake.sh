#!/usr/bin/env sh

#
# build_pdpmake.sh: build pdpmake for Rach2 from the pinned, vendored tarball,
# so the machine can drive a build on itself rather than one command at a time.
#
# There is very little arranging to do here, and that is why this make was
# chosen: pdpmake is nine C files and a 30-line POSIX makefile with no configure
# script, so the cross build is the wrapper, `-static`, and nothing else. tcc's
# port needed a fake cross prefix because its configure RUNS a program it just
# built; there is nothing here to run.
#
# One patch, applied here rather than committed as an edited tree, sets the two
# built-in macros this platform needs: `CC=tcc` and `LDFLAGS=-static`. POSIX
# leaves the values of the built-in macros implementation-defined, so this is
# the port stating its platform rather than a change of behaviour. It must not
# be done through /etc/profile: that file is read by a login shell and not by a
# non-login shell, an SSH command, or a direct exec, so a make started any other
# way would ask tcc for a dynamic link and be refused by tcc's own policy.
#
# The architecture is RACH2_ARCH's (mk/arch.sh); an x86-64 build extracts
# into build/x86_64/vendor-work/pdpmake/ and stages under
# build/x86_64/vendor/pdpmake/.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_pdpmake.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"
cd "$SCRIPT_DIR"

VERSION="2.0.4"
TARBALL="pdpmake-${VERSION}.tgz"
WORK="$ARCH_VENDOR/pdpmake"
SRC="$WORK/pdpmake-${VERSION}"
PREFIX="$MUSL_PREFIX"
WRAP="$SCRIPT_DIR/../musl/muslgcc"
# Out-of-tree staging dir, mirroring the source path vendor/pdpmake/
# (mk/build.mk). Under build/ so a routine `make clean` never reaches it; only
# `make distclean` forces this rebuild.
STAGE="$ARCH_BUILD/vendor/pdpmake"

# Pinned sha256 of the vendored tarball (published on frippery.org/make). The
# build refuses to proceed if the tarball does not match; the version is locked.
TARBALL_SHA256="7e19294d54edf360591d76d3b7a0a511864902ac3a75a0b5b11c7b3cae14c13f"

sha256_of()
{
    make -s -C "$REPO" build/tools/verify/sha256 || return 1
    "$SCRIPT_DIR/../../build/tools/verify/sha256" "$1" | awk '{print $1}'
}

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "pdpmake: $PREFIX/lib/libc.a not found; run vendor/musl/build_libc.sh first" >&2
    exit 1
fi
if [ ! -x "$WRAP" ]; then
    echo "pdpmake: $WRAP not found (the musl compiler wrapper)" >&2
    exit 1
fi
if [ ! -f "$TARBALL" ]; then
    echo "pdpmake: $TARBALL missing (the vendored source)" >&2
    exit 1
fi

echo "pdpmake: verifying $TARBALL"
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "pdpmake: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "pdpmake: extracting into $WORK"
rm -rf "$SRC"
mkdir -p "$WORK"
tar xzf "$TARBALL" -C "$WORK"

# The platform patch is REQUIRED, not discovered. A missing patch directory or
# an empty glob would build quietly and ship a make whose default rules call a
# compiler this image does not have and whose default link this system's
# compiler refuses, and the student would meet that at their first build.
PATCH="patches/0001-platform-macros.patch"
if [ ! -f "$PATCH" ]; then
    echo "pdpmake: $PATCH is missing; it is what points the built-in macros at this platform" >&2
    exit 1
fi
echo "pdpmake: applying $PATCH"
if ! patch -p1 -F 0 --forward -d "$SRC" < "$PATCH"; then
    echo "pdpmake: $PATCH does not apply to pdpmake $VERSION; rebase it" >&2
    exit 1
fi

# And the result is asserted rather than assumed: a patch can apply to the wrong
# place, and these two values are what every implicit rule in every makefile on
# the machine will use.
for want in 'CC=tcc' 'LDFLAGS=-static'; do
    if ! grep -q "\"$want" "$SRC/rules.c"; then
        echo "pdpmake: the patch applied but the built-in macros lack $want" >&2
        exit 1
    fi
done
if grep -q '"CC=c99' "$SRC/rules.c" || grep -q '"CC=c17' "$SRC/rules.c" ||
   grep -q '"CC=cc' "$SRC/rules.c"; then
    echo "pdpmake: a mode still names a compiler this image does not carry" >&2
    exit 1
fi

echo "pdpmake: building (static, musl, $RACH2_ARCH)"
mkdir -p "$STAGE"
(cd "$SRC" && make CC="$WRAP" LDFLAGS="-static" >/dev/null)

cp "$SRC/make" "$STAGE/make"

# Judged against what this kernel will actually load, the way tcc's own build
# judges its output: `file(1)` calls a dynamic binary and a static one the same
# thing, and this loader does not.
echo "pdpmake: checking the result is loadable by this kernel"
ELFCHECK="$SCRIPT_DIR/../../build/tools/inspect/elfcheck"
make -s -C "$REPO" build/tools/inspect/elfcheck || exit 1
"$ELFCHECK" --arch "$RACH2_ARCH" "$STAGE/make"

echo "pdpmake: built $VERSION ($(wc -c < "$STAGE/make") bytes, static, musl, $RACH2_ARCH)"
echo "pdpmake: rebuild the os-real image (make os-real-image) to install it"
