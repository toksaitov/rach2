#!/bin/sh
#
# tools/setup/build_host_tool.sh: build from pinned sources a host tool that
# the Rach2 build needs, for a host whose package manager does not have it.
#
# Usage: sh tools/setup/build_host_tool.sh NAME PREFIX
#
# NAME is one of:
#
#   i686-elf, x86_64-elf  the cross toolchain of that target: binutils, then
#                         GCC for C alone with its libgcc, built with GMP,
#                         MPFR and MPC in GCC's source tree and without ISL
#   e2fsprogs             mke2fs, debugfs and e2fsck, from the tarball
#                         vendored under vendor/e2fsprogs/
#   mandoc                mandoc, linked against a static zlib built with it
#   pmccabe               the recorded 2.8 complexity parser
#
# and it is installed under PREFIX, which is made if absent, with its programs
# in PREFIX/bin. Each upstream tarball is kept in build/downloads/host-tools/
# of the tree for the next build, and used only when its SHA-256 is the one
# pinned below. A tarball the tree lacks is copied from the directory that the
# trees of a host share, RACH2_DOWNLOADS, or /var/cache/rach2-downloads where
# that exists, when the copy there has the pinned SHA-256; otherwise it is
# downloaded with curl or wget and added there. RACH2_DOWNLOADS set empty turns
# the shared directory off. The build runs in
# build/host-tools/NAME/, which is removed when the script exits, whether it
# succeeded or not. It needs a C and a C++ compiler, GNU Make, m4, gzip, bzip2
# and tar.
#
# On Windows the cross toolchain and e2fsprogs use the MSYS runtime. The
# source judges use the native UCRT compiler and need no MSYS runtime when
# executed. With the packages gcc, make, curl, tar,
# xz, bzip2, m4, diffutils and patch installed by pacman, the command is the
# same, run by MSYS2's shell from the tree, for example:
#
#   C:\msys64\usr\bin\bash.exe -lc 'cd /c/src/rach2 &&
#       sh tools/setup/build_host_tool.sh i686-elf /c/rach2-tools'
#
# There config.guess calls the machine x86_64-pc-msys, which the pinned GCC
# does not know as a host; the MSYS runtime is a fork of Cygwin's and its
# compiler defines __CYGWIN__, so each configure is given the build triple
# x86_64-pc-cygwin instead. Neither path may contain a space.
#
set -u

# The pinned sources. Each SHA-256 was computed from a download checked
# another way: binutils and GCC against their signatures and the GNU keyring;
# GMP, MPFR and MPC against the SHA-512 sums in GCC's
# contrib/prerequisites.sha512; mandoc against the sum mandoc.bsd.lv publishes;
# and zlib against its author's signature.
GNU_MIRROR=https://ftp.gnu.org/gnu
GCC_INFRA=https://gcc.gnu.org/pub/gcc/infrastructure

BINUTILS=binutils-2.46.1
BINUTILS_SHA256=364c8faa19ea46c44089c2d59c1fee6eda9a273787d0fe8ab9dfb249069c6aee
GCC=gcc-15.2.0
GCC_SHA256=7294d65cc1a0558cb815af0ca8c7763d86f7a31199794ede3f630c0d1b0a5723
GMP=gmp-6.2.1
GMP_SHA256=eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c
MPFR=mpfr-4.1.0
MPFR_SHA256=feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926
MPC=mpc-1.2.1
MPC_SHA256=17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459
MANDOC=mandoc-1.14.6
MANDOC_SHA256=8bf0d570f01e70a6e124884088870cbed7537f36328d512909eb10cd53179d9c
ZLIB=zlib-1.3.2
ZLIB_SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
PMCCABE=pmccabe_2.8.orig.tar.gz
# Debian's signed pmccabe_2.8-3.dsc records this upstream source digest.
PMCCABE_SHA256=08c125d13e43f4c7557fb15ad953e06562097ed5bae094b9683b265827e2c4dc
STRPTIME_REF=036e40e6d80b1d71e57d3715ae20175ce09d7b78
STRPTIME_SHA256=ecbb2772b734a90ba54e87adb61a8b94f131f300d78ddba4be7ae54e50436e12

die() {
    echo "build-host-tool: $*" >&2
    exit 1
}

say() {
    echo "build-host-tool: $*" >&2
}

usage() {
    echo "usage: sh tools/setup/build_host_tool.sh i686-elf|x86_64-elf|e2fsprogs|mandoc|pmccabe PREFIX" >&2
    exit 2
}

[ $# -eq 2 ] || usage
NAME=$1
case "$NAME" in
    i686-elf|x86_64-elf|e2fsprogs|mandoc|pmccabe) ;;
    *) usage ;;
esac

TREE=$(cd "$(dirname "$0")/../.." && pwd -P) || die "cannot find the tree"
DOWNLOADS=$TREE/build/downloads/host-tools
SHARED=${RACH2_DOWNLOADS-}
if [ -z "${RACH2_DOWNLOADS+set}" ] && [ -d /var/cache/rach2-downloads ]; then
    SHARED=/var/cache/rach2-downloads
fi
mkdir -p "$2" || die "cannot make $2"
PREFIX=$(cd "$2" && pwd -P) || die "cannot enter $2"
case "$TREE$PREFIX" in
    *[[:space:]]*) die "the tree and PREFIX must not contain spaces: $TREE, $PREFIX" ;;
esac

BUILD_OPT=
NATIVE_WINDOWS=0
case "$(uname -s)" in
    MSYS*|MINGW*|UCRT*|CLANG*)
        case "$NAME" in
            mandoc|pmccabe)
                NATIVE_WINDOWS=1
                CC=${SOURCE_TOOL_CC:-/ucrt64/bin/gcc}
                case "$("$CC" -dumpmachine)" in
                    *mingw*) ;;
                    *) die "$CC must produce native Windows executables" ;;
                esac
                export CC ;;
            *)
                case "$(uname -s)" in
                    MSYS*) BUILD_OPT=--build=x86_64-pc-cygwin ;;
                    *) die "run the cross-tool build in MSYS2's MSYS environment" ;;
                esac ;;
        esac ;;
esac

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
case "$JOBS" in
    ''|*[!0-9]*) JOBS=1 ;;
esac
[ "$JOBS" -gt 8 ] && JOBS=8
[ "$JOBS" -lt 1 ] && JOBS=1

# The release tarballs have their generated files; a missing makeinfo must
# not make the build try to regenerate the manuals.
MAKEINFO=true
export MAKEINFO

work=
part=
trap 'rm -rf ${work:+"$work"} ${part:+"$part"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$TREE/build/host-tools" "$DOWNLOADS" || die "cannot make the build directories"
[ -z "$SHARED" ] || mkdir -p "$SHARED" || die "cannot make $SHARED"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        die "neither sha256sum nor shasum found"
    fi
}

# check FILE SHA256: succeed when FILE has that SHA-256.
check() {
    got=$(sha256_of "$1") || exit 1
    [ "$got" = "$2" ] && return 0
    say "sha256 mismatch for $1"
    say "  expected $2"
    say "  got      $got"
    return 1
}

# share FILE [new]: add the checked FILE of DOWNLOADS to SHARED, unless SHARED
# has a file of its name and the second argument is absent. The file is
# written under a name of its own and renamed into place, so that a tree
# reading SHARED meanwhile never sees part of it. Failing to add it is no
# error.
share() {
    [ -n "$SHARED" ] || return 0
    s=$SHARED/${1##*/}
    [ $# -eq 1 ] && [ -f "$s" ] && return 0
    if ! part=$(mktemp "$SHARED/.${1##*/}.XXXXXX") ||
        ! cp "$1" "$part" || ! chmod 664 "$part" || ! mv "$part" "$s"; then
        say "cannot add ${1##*/} to $SHARED"
    fi
    rm -f "$part"
    part=
}

# fetch URL SHA256: put the file URL names in DOWNLOADS, checked against
# SHA256, from SHARED when it has the file and otherwise from URL. A partial
# or wrong file never takes the file's name.
fetch() {
    f=$DOWNLOADS/${1##*/}
    if [ -f "$f" ]; then
        check "$f" "$2" || die "remove $f and run again"
        share "$f"
        return 0
    fi
    s=$SHARED/${1##*/}
    if [ -n "$SHARED" ] && [ -f "$s" ]; then
        if check "$s" "$2"; then
            say "copying ${1##*/} from $SHARED"
            rm -f "$f.part"
            cp "$s" "$f.part" && mv "$f.part" "$f" && return 0
            rm -f "$f.part"
            die "cannot copy $s"
        fi
    fi
    say "downloading $1"
    rm -f "$f.part"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$f.part" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$f.part" "$1"
    else
        die "neither curl nor wget found"
    fi || { rm -f "$f.part"; die "cannot download $1"; }
    check "$f.part" "$2" || { rm -f "$f.part"; die "refusing $1"; }
    mv "$f.part" "$f" || die "cannot rename $f.part"
    share "$f" new
}

# unpack TARBALL DIR: extract TARBALL, compressed by gzip or bzip2, into DIR.
unpack() {
    say "extracting ${1##*/}"
    case "$1" in
        *.gz)  gzip -dc "$1" ;;
        *.bz2) bzip2 -dc "$1" ;;
        *)     die "unknown compression: $1" ;;
    esac | (cd "$2" && tar -xf -) || die "cannot extract $1"
}

# run DIR CMD...: run CMD in DIR, and stop the script if it fails.
run() {
    dir=$1
    shift
    (cd "$dir" && "$@") || die "failed in $dir: $*"
}

# place TARBALL DIR NAME: extract a library TARBALL that unpacks to DIR, and
# move DIR into GCC's source tree as NAME.
place() {
    unpack "$DOWNLOADS/$1" "$work/src"
    mv "$work/src/$2" "$work/src/$GCC/$3" || die "cannot place $2"
}

build_cross() {
    target=$1
    fetch "$GNU_MIRROR/binutils/$BINUTILS.tar.gz" "$BINUTILS_SHA256"
    fetch "$GNU_MIRROR/gcc/$GCC/$GCC.tar.gz" "$GCC_SHA256"
    fetch "$GCC_INFRA/$GMP.tar.bz2" "$GMP_SHA256"
    fetch "$GCC_INFRA/$MPFR.tar.bz2" "$MPFR_SHA256"
    fetch "$GCC_INFRA/$MPC.tar.gz" "$MPC_SHA256"

    mkdir "$work/src" "$work/binutils" "$work/gcc" || die "cannot make $work"
    unpack "$DOWNLOADS/$BINUTILS.tar.gz" "$work/src"
    unpack "$DOWNLOADS/$GCC.tar.gz" "$work/src"
    # Where contrib/download_prerequisites puts them, without its symlinks:
    # GCC's configure builds a library found under these names in its tree.
    place "$GMP.tar.bz2" "$GMP" gmp
    place "$MPFR.tar.bz2" "$MPFR" mpfr
    place "$MPC.tar.gz" "$MPC" mpc

    # Binutils first: the GCC build assembles and archives libgcc with them.
    PATH=$PREFIX/bin:$PATH
    export PATH
    say "configuring $BINUTILS for $target"
    run "$work/binutils" "$work/src/$BINUTILS/configure" ${BUILD_OPT:+"$BUILD_OPT"} \
        --target="$target" --prefix="$PREFIX" --with-sysroot \
        --disable-nls --disable-werror --disable-gprofng
    say "building $BINUTILS for $target"
    run "$work/binutils" make -j"$JOBS"
    run "$work/binutils" make install
    say "configuring $GCC for $target"
    run "$work/gcc" "$work/src/$GCC/configure" ${BUILD_OPT:+"$BUILD_OPT"} \
        --target="$target" --prefix="$PREFIX" --without-headers \
        --enable-languages=c --disable-nls --disable-werror --without-isl
    say "building $GCC for $target"
    run "$work/gcc" make -j"$JOBS" all-gcc all-target-libgcc
    run "$work/gcc" make install-gcc install-target-libgcc
}

# put DIR PROGRAM...: copy each built PROGRAM in DIR to PREFIX/bin, with the
# .exe suffix the compiler gave it on Windows.
put() {
    d=$1
    shift
    mkdir -p "$PREFIX/bin" || die "cannot make $PREFIX/bin"
    for p in "$@"; do
        if [ -f "$d/$p.exe" ]; then
            cp "$d/$p.exe" "$PREFIX/bin/$p.exe"
        else
            cp "$d/$p" "$PREFIX/bin/$p"
        fi || die "cannot install $p"
    done
}

build_e2fsprogs() {
    # The version and the pinned sum are the vendored build's, read from it.
    script=$TREE/vendor/e2fsprogs/build_e2fsprogs.sh
    version=$(sed -n 's/^VERSION="\([0-9.]*\)"$/\1/p' "$script")
    sum=$(sed -n 's/^TARBALL_SHA256="\([0-9a-f]*\)"$/\1/p' "$script")
    [ -n "$version" ] && [ -n "$sum" ] || die "cannot read the pinned version from $script"
    tarball=$TREE/vendor/e2fsprogs/e2fsprogs-$version.tar.gz
    [ -f "$tarball" ] || die "$tarball missing"
    check "$tarball" "$sum" || die "refusing $tarball"

    mkdir "$work/src" "$work/e2fsprogs" || die "cannot make $work"
    unpack "$tarball" "$work/src"
    b=$work/e2fsprogs
    say "configuring e2fsprogs-$version"
    run "$b" "$work/src/e2fsprogs-$version/configure" ${BUILD_OPT:+"$BUILD_OPT"} \
        --prefix="$PREFIX" --disable-nls --disable-fuse2fs --disable-uuidd \
        --disable-defrag --disable-imager --disable-resizer \
        --disable-backtrace --disable-elf-shlibs --disable-bsd-shlibs \
        --enable-libuuid --enable-libblkid --without-libarchive
    say "building e2fsprogs-$version"
    run "$b" make -j"$JOBS" top-deps
    run "$b" make -j"$JOBS" libs
    run "$b/misc" make -j"$JOBS" mke2fs
    run "$b/debugfs" make -j"$JOBS" debugfs
    run "$b/e2fsck" make -j"$JOBS" e2fsck
    put "$b/misc" mke2fs
    put "$b/debugfs" debugfs
    put "$b/e2fsck" e2fsck
}

build_mandoc() {
    fetch "https://zlib.net/fossils/$ZLIB.tar.gz" "$ZLIB_SHA256"
    fetch "https://mandoc.bsd.lv/snapshots/$MANDOC.tar.gz" "$MANDOC_SHA256"

    mkdir "$work/src" || die "cannot make $work"
    unpack "$DOWNLOADS/$ZLIB.tar.gz" "$work/src"
    unpack "$DOWNLOADS/$MANDOC.tar.gz" "$work/src"
    z=$work/src/$ZLIB
    m=$work/src/$MANDOC
    # Only the static libz.a is built, and mandoc links it from its tree.
    say "building $ZLIB"
    run "$z" ./configure --static
    run "$z" make -j"$JOBS" libz.a
    # mandoc's configure reads its settings from configure.local.
    say "configuring $MANDOC"
    {
        echo "PREFIX=$PREFIX"
        echo "CC=${CC:-cc}"
        if [ "$NATIVE_WINDOWS" = 1 ]; then
            compat=$TREE/tools/setup/mandoc-win
            native_z=$(cygpath -m "$z") || die "cannot convert zlib path"
            native_compat=$(cygpath -m "$compat") || die "cannot convert compatibility path"
            echo "CFLAGS=\"-g -O2 -std=gnu99 -D_GNU_SOURCE -I$native_z -I$native_compat -include $native_compat/strptime.h\""
            echo 'HAVE_STRPTIME=1'
            echo 'OSNAME=Windows'
            echo "LDFLAGS=\"-L$native_z\""
        else
            echo "CFLAGS=\"-g -O2 -I$z\""
            echo "LDFLAGS=\"-L$z\""
        fi
    } >"$m/configure.local" || die "cannot write $m/configure.local"
    mkdir -p "$TREE/build/host-tool-logs/mandoc" || die "cannot make log directory"
    (cd "$m" && ./configure)
    configured=$?
    cp "$m/config.log" "$TREE/build/host-tool-logs/mandoc/config.log" || die "cannot save config.log"
    [ "$configured" = 0 ] || die "mandoc configure failed; see build/host-tool-logs/mandoc/config.log"
    say "building $MANDOC"
    if [ "$NATIVE_WINDOWS" = 1 ]; then
        fetch "https://raw.githubusercontent.com/vim/vim/$STRPTIME_REF/src/strptime.c" "$STRPTIME_SHA256"
        cp "$DOWNLOADS/strptime.c" "$m/batch_strptime.c" || die "cannot copy strptime"
        cp "$compat/main.c" "$m/batch_main.c" || die "cannot copy batch entry"
        cp "$compat/mmap.c" "$m/batch_mmap.c" || die "cannot copy mapping adapter"
        run "$m" make -f "$TREE/tools/setup/mandoc-batch.mk" -j"$JOBS" mandoc
        mkdir -p "$PREFIX/share/licenses" || die "cannot make license directory"
        cp "$DOWNLOADS/strptime.c" "$PREFIX/share/licenses/mandoc-strptime.c" || die "cannot copy license"
    else
        run "$m" make -j"$JOBS" mandoc
    fi
    put "$m" mandoc
    mkdir -p "$PREFIX/share/licenses" || die "cannot make license directory"
    cp "$m/LICENSE" "$PREFIX/share/licenses/mandoc.txt" || die "cannot copy mandoc license"
    cp "$z/LICENSE" "$PREFIX/share/licenses/zlib.txt" || die "cannot copy zlib license"
    tool_record mandoc
}

tool_record() {
    python3 "$TREE/tests/system/source_tools_test.py" check "$1" "$PREFIX" ||
        die "$1 fixture failed; evidence: $PREFIX/$1.check.json"
    python3 "$TREE/tools/setup/source_tool_record.py" write "$1" "$PREFIX" ||
        die "cannot write tool record"
}

build_pmccabe() {
    fetch "https://deb.debian.org/debian/pool/main/p/pmccabe/$PMCCABE" "$PMCCABE_SHA256"
    mkdir "$work/src" || die "cannot make $work"
    unpack "$DOWNLOADS/$PMCCABE" "$work/src"
    pm_source=$work/src/pmccabe-v2.8
    flags='-std=gnu89 -O2 -Wall -Wextra'
    [ "$NATIVE_WINDOWS" = 0 ] || flags="$flags -DWIN32"
    run "$pm_source" make CC="${CC:-cc}" CFLAGS="$flags" pmccabe
    put "$pm_source" pmccabe
    mkdir -p "$PREFIX/share/licenses" || die "cannot make license directory"
    cp "$pm_source/COPYING" "$PREFIX/share/licenses/pmccabe.txt" || die "cannot copy pmccabe license"
    tool_record pmccabe
}

case "$NAME" in
    pmccabe|mandoc)
        if python3 "$TREE/tools/setup/source_tool_record.py" check "$NAME" "$PREFIX"; then
            say "$NAME is unchanged in $PREFIX"
            exit 0
        fi ;;
esac
mkdir "$TREE/build/host-tools/$NAME" ||
    die "$TREE/build/host-tools/$NAME exists; another build is running there, or remove it"
work=$TREE/build/host-tools/$NAME

case "$NAME" in
    i686-elf|x86_64-elf) build_cross "$NAME" ;;
    e2fsprogs) build_e2fsprogs ;;
    mandoc) build_mandoc ;;
    pmccabe) build_pmccabe ;;
esac
say "installed $NAME in $PREFIX"
