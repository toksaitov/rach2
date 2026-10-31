#!/usr/bin/env sh

#
# build_libtsm.sh: build libtsm, the terminal emulator's state machine that
# term draws, as a static library from the pinned, vendored tarball, for one
# architecture of Rach2 or for the host.
#
# libtsm builds with Meson, which this build does not need: the library is
# six C files, a hash table and a wcwidth, compiled here one by one and put
# in libtsm.a with the patched libtsm.h beside it. Its hash table,
# src/shared/shl-htable.c and its header, is LGPL 2.1 with inline code in the
# header; both are removed after the extract and Rach2's own, shl-htable.c and
# shl-htable.h beside this script, take their place, so that every file
# compiled is MIT or public domain. The script stops if any file of the tree
# still carries the LGPL's notice.
#
# wcwidth.c defines wcwidth(), which musl's libc.a defines too; it is compiled
# as tsm_wcwidth, by a macro given to every file, so that a program linked
# with both keeps musl's and libtsm keeps its own, as libtsm's version script
# keeps it private in a shared build.
#
# The patches patches/series names are applied in its order with
# `patch -F 0 --forward`, as vendor/pdpmake's is, so that a patch whose
# context moved fails instead of applying in the wrong place.
#
# A Rach2 build extracts into vendor/libtsm/, or build/x86_64/vendor-work/libtsm/
# for x86-64 (mk/arch.sh), compiles with the musl wrapper and stages under
# build/vendor/libtsm/ or build/x86_64/vendor/libtsm/, with the archive of the
# patched sources that term's guest build compiles. A host build extracts
# into build/host/vendor-work/libtsm/, compiles with HOSTCC, cc unless set,
# and stages under build/host/vendor/libtsm/. Each stage has lib/libtsm.a and
# include/libtsm.h.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_libtsm.sh
#        ./build_libtsm.sh --host
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

VERSION="4.8.0"
TARBALL="libtsm-${VERSION}.tar.gz"
# Pinned sha256 of the vendored tarball, GitHub's archive of the tag v4.8.0 of
# github.com/kmscon/libtsm. The build refuses to proceed if the tarball does
# not match.
TARBALL_SHA256="50811e9e94fc798eb5dcbaff20f81067c942ec47f329f91bbe4caeb35a61ff05"

if [ "${1-}" = "--host" ]; then
    WORK="$REPO/build/host/vendor-work/libtsm"
    STAGE="$REPO/build/host/vendor/libtsm"
    CC="${HOSTCC:-cc}"
    AR="ar"
    WHAT="host"
else
    . "$REPO/mk/arch.sh"
    WORK="$ARCH_VENDOR/libtsm"
    STAGE="$ARCH_BUILD/vendor/libtsm"
    CC="$SCRIPT_DIR/../musl/muslgcc"
    AR="${CROSS}ar"
    WHAT="musl, $RACH2_ARCH"
    if [ ! -f "$MUSL_PREFIX/lib/libc.a" ]; then
        echo "libtsm: $MUSL_PREFIX/lib/libc.a not found; run make musl in the top directory first" >&2
        exit 1
    fi
fi
SRC="$WORK/libtsm-${VERSION}"
OBJ="$WORK/obj"
cd "$SCRIPT_DIR"

sha256_of()
{
    make -s -C "$REPO" build/tools/verify/sha256 || return 1
    "$REPO/build/tools/verify/sha256" "$1" | awk '{print $1}'
}

if [ ! -f "$TARBALL" ]; then
    echo "libtsm: $TARBALL missing (the vendored source)" >&2
    exit 1
fi

echo "libtsm: verifying $TARBALL"
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "libtsm: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "libtsm: extracting into $WORK"
rm -rf "$SRC" "$OBJ"
mkdir -p "$WORK" "$OBJ"
tar xzf "$TARBALL" -C "$WORK"

echo "libtsm: replacing the LGPL hash table with Rach2's"
rm -f "$SRC/LICENSE_htable" "$SRC/src/shared/shl-htable.c" "$SRC/src/shared/shl-htable.h"
cp shl-htable.c shl-htable.h "$SRC/src/shared/"
if grep -rliE 'GNU (Lesser|Library) General Public|LGPLv?[0-9]|SPDX-License-Identifier:.*LGPL' "$SRC" >&2; then
    echo "libtsm: the files above are under the LGPL" >&2
    exit 1
fi

while IFS= read -r p; do
    case "$p" in ''|'#'*) continue ;; esac
    if [ ! -f "patches/$p" ]; then
        echo "libtsm: patches/series names a missing patch: $p" >&2
        exit 1
    fi
    echo "libtsm: applying patches/$p"
    if ! patch -p1 -F 0 --forward -d "$SRC" < "patches/$p" >/dev/null; then
        echo "libtsm: patches/$p does not apply; rebase it" >&2
        exit 1
    fi
done < patches/series
for p in patches/*.patch; do
    [ -e "$p" ] || break
    if ! grep -qx "$(basename "$p")" patches/series; then
        echo "libtsm: $p is not named in patches/series; add it there or remove it" >&2
        exit 1
    fi
done
if ! grep -q "^#define TSM_VERSION \"$VERSION\"$" "$SRC/src/tsm/libtsm.h"; then
    echo "libtsm: the patched libtsm.h does not define TSM_VERSION as \"$VERSION\"" >&2
    exit 1
fi

# Meson's flags for the project (meson.build), less its -Werror, which a
# compiler newer than upstream's would turn into a failed build, and with no
# config.h, whose one setting, BUILD_ENABLE_DEBUG, is off by being undefined.
CFLAGS="-std=gnu99 -O2 -Wall -ffast-math -fno-strict-aliasing -D_GNU_SOURCE \
-D_POSIX_C_SOURCE=200809L -Dwcwidth=tsm_wcwidth \
-I$SRC/src/tsm -I$SRC/src/shared -I$SRC/external -I$SRC/external/wcwidth"
# The files compiled, from sources.mk, which term's guest build reads too.
SOURCES="$(sed -n 's/^LIBTSM_SRC = //p' sources.mk | sed 's|\$(LIBTSM)/||g')"
if [ -z "$SOURCES" ]; then
    echo "libtsm: sources.mk has no LIBTSM_SRC line" >&2
    exit 1
fi

echo "libtsm: building ($WHAT)"
OBJECTS=
for s in $SOURCES; do
    o="$OBJ/$(basename "$s" .c).o"
    # shellcheck disable=SC2086 # CFLAGS is a list of words
    "$CC" $CFLAGS -c "$SRC/$s" -o "$o"
    OBJECTS="$OBJECTS $o"
done

rm -rf "$STAGE"
mkdir -p "$STAGE/lib" "$STAGE/include"
# shellcheck disable=SC2086 # OBJECTS is a list of paths without spaces
"$AR" rcs "$STAGE/lib/libtsm.a" $OBJECTS
cp "$SRC/src/tsm/libtsm.h" "$STAGE/include/libtsm.h"

# For Rach2, the archive of the patched sources that the images stage as
# /src/term/vendor/libtsm.tar.bz2, which term/guest.mk unpacks and compiles
# with TinyCC: the files compiled, the headers of the directories they
# include from, and the two license files, in name order.
if [ "$WHAT" != host ]; then
    (cd "$SRC" && {
        printf '%s\n' $SOURCES COPYING external/wcwidth/LICENSE.txt
        find src/tsm src/shared external/wcwidth external/xkbcommon -name '*.h'
    } | LC_ALL=C sort -u) > "$OBJ/list"
    case "$(tar --version 2>/dev/null)" in
        *bsdtar*) set -- --no-xattrs --no-mac-metadata ;;
        *) set -- ;;
    esac
    COPYFILE_DISABLE=1 tar -c -f "$OBJ/libtsm.tar" "$@" -C "$SRC" -T "$OBJ/list"
    bzip2 -9 "$OBJ/libtsm.tar"
    mv "$OBJ/libtsm.tar.bz2" "$STAGE/libtsm.tar.bz2"
fi

echo "libtsm: built $VERSION ($(wc -c < "$STAGE/lib/libtsm.a") bytes, $WHAT)"
