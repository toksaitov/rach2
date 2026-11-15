#!/bin/sh
#
# build_igt.sh: build IGT's tests from Debian's intel-gpu-tools 2.0 source in
# the Debian 13 builder guest, for the x86-64 GNU root.
#
# Usage: build_igt.sh
#        build_igt.sh guest
#
# Debian's intel-gpu-tools package has IGT's tools but not its tests or its
# runner, since its rules configure with -Dtests=disabled. On the host, the
# script fetches the source package that sources.txt pins and checks each
# file's SHA-256, prepares the builder guest of tests/graphics/debian-ref if
# this clone has none, and boots it with the four files, the patch
# kms-content-protection-plane-size.patch and itself, which the guest runs
# as `build_igt.sh guest`. There the patch is applied to the unpacked source,
# and Debian's own debhelper configures, builds and installs it with the
# flags of Debian's build, as debian/rules does, and these changes: the
# tests and the runner are built, the libraries the tests use go in
# /usr/libexec/igt-gpu-tools/lib, which the tests and the runner find by
# their run paths, so that nothing replaces the packaged libigt.so.0, and
# Chamelium, liboping, Valgrind and the documentation are off, so that the
# result does not depend on what else the guest has installed. The patch
# carries upstream IGT's later fix to kms_content_protection, whose primary
# plane IGT 2.0 scales when a second connected output is larger than the
# first.
#
# The guest stages what the package lacks: the tests, their libraries and
# lists under /usr/libexec/igt-gpu-tools, less the benchmarks and Intel's CI
# lists that the package has; the runner's programs igt_runner, igt_resume,
# igt_results and igt_comms_decoder in /usr/bin; and the tests' images in
# /usr/share/igt-gpu-tools. It records every subtest, as `igt_runner
# --list-all` gives them, in /usr/libexec/igt-gpu-tools/subtest-list.txt,
# which the same command on Rach2 must repeat.
#
# The stage goes in igt/ of the GNU build tree, which install_root.sh
# copies into the root, and the build's log in work/igt/build.log, which
# ends with the subtest list; the run's record, the builder's QEMU and
# guest, is in work/igt/guest.record. GNU_IGT_CPUS and GNU_IGT_MIB give the
# guest's processors and memory, 8 and 8192.
#

set -e

if [ "${1:-}" = guest ]; then
    set -u
    say()
    {
        echo "igt: $*"
    }
    x=usr/libexec/igt-gpu-tools
    # Debian's /tmp is a tmpfs of half the memory, too small for the build.
    work=/var/tmp/igt
    rm -rf "$work"
    mkdir -p "$work"
    cd "$work"
    say "unpacking intel-gpu-tools 2.0-1"
    dpkg-source -x /tmp/intel-gpu-tools_2.0-1.dsc src
    cd src
    say "applying kms-content-protection-plane-size.patch"
    patch -p1 --no-backup-if-mismatch < /tmp/kms-content-protection-plane-size.patch
    say "installing the build dependencies"
    sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y build-dep ./
    sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install libjson-c-dev
    DEB_BUILD_OPTIONS="parallel=$(nproc)"
    export DEB_BUILD_OPTIONS
    say "configuring"
    dh_auto_configure --buildsystem=meson --builddirectory=build -- \
        -Dsrcdir=. -Dtests=enabled -Drunner=enabled -Duse_rpath=true \
        --libdir=libexec/igt-gpu-tools/lib -Dchamelium=disabled -Doping=disabled \
        -Dvalgrind=disabled -Ddocs=disabled -Dtestplan=disabled -Dman=disabled
    say "building"
    dh_auto_build --buildsystem=meson --builddirectory=build
    dh_auto_install --buildsystem=meson --builddirectory=build --destdir="$work/dest"
    dest=$work/dest
    stage=$work/stage
    mkdir -p "$stage/usr/bin" "$stage/usr/share/igt-gpu-tools" "$stage/usr/libexec"
    cp -a "$dest/$x" "$stage/$x"
    # The package has these; the lib directory keeps only the sonames.
    rm -rf "$stage/$x/benchmarks" "$stage/$x/intel-ci-tests" "$stage/$x/lib/pkgconfig"
    find "$stage/$x/lib" -name '*.so' -type l -exec rm {} +
    for p in igt_runner igt_resume igt_results igt_comms_decoder; do
        cp -a "$dest/usr/bin/$p" "$stage/usr/bin/$p"
    done
    cp -a "$dest/usr/share/igt-gpu-tools"/*.png "$stage/usr/share/igt-gpu-tools/"
    "$stage/usr/bin/igt_runner" --allow-non-root --list-all "$stage/$x" > "$work/subtests"
    cp "$work/subtests" "$stage/$x/subtest-list.txt"
    tar -c -f /tmp/igt.tar --owner=0 --group=0 --numeric-owner -C "$stage" .
    say "left out of the stage: $(cd "$dest" && find . ! -type d |
        grep -c -v -e "^\./$x/" -e '^\./usr/bin/igt_\(runner\|resume\|results\|comms_decoder\)$' \
            -e '^\./usr/share/igt-gpu-tools/[^/]*\.png$') files of the install"
    for p in gcc meson libdrm-dev libcairo2-dev libglib2.0-dev libjson-c-dev libkmod-dev \
        libproc2-dev libudev-dev libunwind-dev libdw-dev libpciaccess-dev libpixman-1-dev; do
        say "version: $p $(dpkg-query -W -f '${Version}' "$p")"
    done
    say "flags: CFLAGS $(dpkg-buildflags --get CFLAGS)"
    say "flags: LDFLAGS $(dpkg-buildflags --get LDFLAGS)"
    find "$stage" -type f -exec sh -c 'readelf -d "$1" 2>/dev/null' sh {} \; |
        sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p' | LC_ALL=C sort -u > "$work/needed"
    (cd "$stage/$x/lib" && ls) | LC_ALL=C sort > "$work/own"
    LC_ALL=C comm -23 "$work/needed" "$work/own" | sed 's/^/igt: needs: /'
    say "result: $(sed '1d;$d' "$stage/$x/test-list.txt" | wc -w | tr -d ' ') tests in test-list.txt," \
        "$(find "$stage/$x" -type f -perm -u+x ! -path "*/lib/*" | wc -l | tr -d ' ') test programs," \
        "$(wc -l < "$work/subtests" | tr -d ' ') subtests"
    say "subtests:"
    cat "$work/subtests"
    exit 0
fi

GNU=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read REPO
REPO=$(cd "$GNU/../.." && pwd)
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
if [ "$RACH2_ARCH" != x86_64 ]; then
    echo "gnu: igt: refused: IGT's tests are built for x86-64 alone" >&2
    exit 1
fi
ROOT="$GNU_OUT/igt"
WORK="$GNU_OUT/work/igt"
rm -rf "$ROOT" "$WORK"
mkdir -p "$ROOT" "$WORK"
pins="$GNU/igt/sources.txt"
dsc=$(asset intel-gpu-tools-dsc "$pins")
orig=$(asset intel-gpu-tools-orig "$pins")
asc=$(asset intel-gpu-tools-orig-asc "$pins")
debian=$(asset intel-gpu-tools-debian "$pins")
ref="$REPO/tests/graphics/debian-ref/debian-ref.sh"
sh "$ref" prepare -n
echo "gnu: igt: building in the builder guest; the log is $WORK/build.log"
if ! sh "$ref" run -c "${GNU_IGT_CPUS:-8}" -m "${GNU_IGT_MIB:-8192}" -r "$WORK/guest.record" \
    -i "$dsc" -i "$orig" -i "$asc" -i "$debian" \
    -i "$GNU/igt/kms-content-protection-plane-size.patch" -i "$GNU/igt/build_igt.sh" -o "$WORK/igt.tar" \
    'sh /tmp/build_igt.sh guest' > "$WORK/build.log" 2>&1; then
    tail -n 40 "$WORK/build.log" >&2
    echo "gnu: igt: the build in the builder guest failed; the log is $WORK/build.log" >&2
    exit 1
fi
tar -x -f "$WORK/igt.tar" -C "$ROOT"
rm -f "$WORK/igt.tar"
x="$ROOT/usr/libexec/igt-gpu-tools"
[ -x "$ROOT/usr/bin/igt_runner" ] && [ -s "$x/test-list.txt" ] && [ -s "$x/subtest-list.txt" ] || {
    echo "gnu: igt: the stage has no runner, test list or subtest list" >&2
    exit 1
}
grep '^igt: needs: ' "$WORK/build.log" | sed 's/^igt: /gnu: igt: /'
echo "gnu: igt: result: $(grep '^igt: result: ' "$WORK/build.log" | sed 's/^igt: result: //')"
