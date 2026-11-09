#!/bin/sh
#
# build_vk_gl_cts.sh: build VK-GL-CTS 1.4.1.1 and 1.4.7.1 for the surfaceless
# EGL target in the Debian 13 builder guest, for the x86-64 GNU root.
#
# Usage: build_vk_gl_cts.sh
#        build_vk_gl_cts.sh guest
#
# On the host, the script fetches every file that sources.txt pins and checks
# its SHA-256, prepares the builder guest of tests/graphics/debian-ref if this
# clone has none, and boots it with the files, the pins and itself, which the
# guest runs as `build_vk_gl_cts.sh guest`. There, for each release, it
# unpacks the release and puts each external source where
# external/fetch_sources.py would have cloned it, configures with CMake for
# DEQP_TARGET=surfaceless, builds deqp-vk and glcts with Ninja against
# Debian's EGL, GLES, zlib and libpng, and stages under
# /opt/vk-gl-cts-VERSION:
#
#   vulkan/deqp-vk     with its data, vulkan/vulkan
#   gl/glcts           with its data: gl_cts, less its mustpass lists, and
#                      the dEQP-GLES2, GLES3 and GLES31 data gles2, gles3
#                      and gles31
#   mustpass/*.txt.gz  the case lists the runner reads, compressed:
#                      vk-default, the Vulkan list with its includes
#                      expanded, and gles2-main, gles3-main and gles31-main,
#                      the AOSP lists
#   licenses/          the licenses of the release and of each external
#                      source, as their trees have them
#   build-info         the release, the pins, the options, the compiler, the
#                      length of each list, and a case of each program run
#                      once in the guest on llvmpipe and lavapipe
#
# 1.4.1.1, Mesa 25.0.7's CI pin, builds with its Vulkan video tests, as Mesa
# builds it; its CMake stops when the validation layers' source is absent,
# so that stop is made a message. 1.4.7.1, the Lenovo's references, builds
# with DEQP_DISABLE_VK_VIDEO_TESTS, as the references were built, and leaves
# the absent layers out by itself.
#
# The stage goes in vk-gl-cts/ of the GNU build tree, which install_root.sh
# copies into the root, and the guest's log in work/vk-gl-cts/build.log; the
# run's record, the builder's QEMU and guest, is in
# work/vk-gl-cts/guest.record. GNU_VK_GL_CTS_CPUS and GNU_VK_GL_CTS_MIB give
# the guest's processors and memory, 16 and 24576.
#

set -e

VERSIONS="1.4.1.1 1.4.7.1"

if [ "${1:-}" = guest ]; then
    set -u
    say()
    {
        echo "vk-gl-cts: $*"
    }
    die()
    {
        echo "vk-gl-cts: $*" >&2
        exit 1
    }
    pins=/tmp/sources.txt
    # Debian's /tmp is a tmpfs of half the memory, too small for the build.
    work=/var/tmp/vk-gl-cts
    stage=$work/stage
    rm -rf "$work"
    mkdir -p "$stage"
    say "installing the build dependencies"
    sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install cmake ninja-build pkgconf \
        libegl-dev libgles-dev zlib1g-dev libpng-dev
    cpus=$(nproc)
    mib=$(awk '$1 == "MemTotal:" { print int($2 / 1024) }' /proc/meminfo)
    # A compiler of the Vulkan tests takes up to some 1.5 GiB.
    jobs=$((mib / 1536))
    [ "$jobs" -le "$cpus" ] || jobs=$cpus
    [ "$jobs" -ge 1 ] || jobs=1

    # expand DIR FILE: the cases of a mustpass list, its includes expanded
    # in place, as deqp-vk reads them.
    expand()
    {
        awk -v d="$1" '
            function ex(f,    l) {
                while ((getline l < f) > 0) {
                    if (l == "" || l ~ /^#/)
                        continue
                    if (l ~ /\.txt$/)
                        ex(d "/" l)
                    else
                        print l
                }
                close(f)
            }
            BEGIN { ex(d "/" ARGV[1]); exit }' "$2"
    }

    for v in $VERSIONS; do
        src=$work/src
        build=$work/build
        o=$stage/opt/vk-gl-cts-$v
        rm -rf "$src" "$build"
        mkdir -p "$src"
        say "unpacking $v"
        url=$(awk -v n="vk-gl-cts-$v" '$1 == n { print $3 }' "$pins")
        tar -x -z -f "/tmp/${url##*/}" -C "$src" --strip-components=1
        awk -v v="$v" '$1 ~ ("-" v "$") && $1 != ("vk-gl-cts-" v) { print $1, $2, $3 }' "$pins" |
            while read -r name commit url; do
                d=$src/external/${name%-"$v"}/src
                mkdir -p "$d"
                tar -x -z -f "/tmp/${url##*/}" -C "$d" --strip-components=1
                say "external: ${name%-"$v"} $commit"
            done
        case $v in
            1.4.1.1)
                sed -i 's/message(FATAL_ERROR "vulkan-validationlayers not found")/message(STATUS "vulkan-validationlayers left out")/' \
                    "$src/external/vulkan-validationlayers/CMakeLists.txt"
                opts=
                ;;
            *)
                opts=-DDEQP_DISABLE_VK_VIDEO_TESTS=ON
                ;;
        esac
        say "configuring $v"
        # shellcheck disable=SC2086 # opts is empty or one word
        cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=surfaceless $opts
        say "building $v with $jobs jobs"
        ninja -C "$build" -j "$jobs" deqp-vk glcts
        vk=$build/external/vulkancts/modules/vulkan
        gl=$build/external/openglcts/modules
        mkdir -p "$o/vulkan" "$o/gl" "$o/mustpass"
        cp "$vk/deqp-vk" "$o/vulkan/"
        cp -RL "$vk/vulkan" "$o/vulkan/vulkan"
        cp "$gl/glcts" "$o/gl/"
        for d in gl_cts gles2 gles3 gles31; do
            cp -RL "$gl/$d" "$o/gl/$d"
        done
        rm -rf "$o/gl/gl_cts/data/mustpass"
        mkdir -p "$o/licenses"
        cp "$src/LICENSE" "$o/licenses/vk-gl-cts"
        for d in "$src"/external/*/src; do
            n=$(basename "$(dirname "$d")")
            for f in "$d"/LICENSE* "$d"/COPYING* "$d"/license*; do
                [ ! -f "$f" ] || cp "$f" "$o/licenses/$n-$(basename "$f")"
            done
        done
        strip "$o/vulkan/deqp-vk" "$o/gl/glcts"
        expand "$src/external/vulkancts/mustpass/main" vk-default.txt | gzip -n -9 > "$o/mustpass/vk-default.txt.gz"
        aosp=$src/external/openglcts/data/gl_cts/data/mustpass/gles/aosp_mustpass/main
        for s in gles2 gles3 gles31; do
            gzip -n -9 < "$aosp/$s-main.txt" > "$o/mustpass/$s-main.txt.gz"
        done
        {
            echo "release vulkan-cts-$v"
            awk -v v="$v" '$1 ~ ("-" v "$") { print "pin", $1, $2, $4 }' "$pins"
            echo "cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=surfaceless${opts:+ $opts}"
            echo "compiler $(c++ --version | head -n 1)"
            for p in libegl-dev libgles-dev zlib1g-dev libpng-dev cmake; do
                echo "package $p $(dpkg-query -W -f '${Version}' "$p")"
            done
            for l in vk-default gles2-main gles3-main gles31-main; do
                echo "list $l $(gzip -d < "$o/mustpass/$l.txt.gz" | wc -l | tr -d ' ') cases"
            done
        } > "$o/build-info"
        # One case of each program in the guest, from its staged directory,
        # as the runner runs it: GLES on llvmpipe through EGL's surfaceless
        # platform, Vulkan on lavapipe.
        say "checking $v"
        (cd "$o/gl" && EGL_PLATFORM=surfaceless ./glcts -n dEQP-GLES2.info.renderer \
            --deqp-surface-type=pbuffer --deqp-gl-config-name=rgba8888d24s8ms0 \
            --deqp-surface-width=256 --deqp-surface-height=256 --deqp-log-filename=/dev/null) > "$work/gl.out" 2>&1 || true
        (cd "$o/vulkan" && ./deqp-vk -n dEQP-VK.api.smoke.create_sampler \
            --deqp-log-filename=/dev/null) > "$work/vk.out" 2>&1 || true
        cat "$work/gl.out" "$work/vk.out"
        for p in gl vk; do
            r=$(grep -E '^  [A-Za-z]+ \(' "$work/$p.out" | head -n 1 | awk '{ print $1 }')
            echo "check $p ${r:-none}" >> "$o/build-info"
            [ "$r" = Pass ] || die "$v: the $p check gave ${r:-no result}"
        done
        rm -rf "$src" "$build"
        say "built $v: $(du -s -k "$o" | awk '{ print $1 }') KiB staged"
        cat "$o/build-info"
    done
    tar -c -f /tmp/vk-gl-cts.tar --owner=0 --group=0 --numeric-owner -C "$stage" .
    find "$stage" -type f -perm -u+x -exec sh -c 'readelf -d "$1" 2>/dev/null' sh {} \; |
        sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p' | LC_ALL=C sort -u | sed 's/^/vk-gl-cts: needs: /'
    say "result: $(for v in $VERSIONS; do printf '%s ' "$v"; done)staged"
    exit 0
fi

GNU=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read REPO
REPO=$(cd "$GNU/../.." && pwd)
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
if [ "$RACH2_ARCH" != x86_64 ]; then
    echo "gnu: vk-gl-cts: refused: VK-GL-CTS is built for x86-64 alone" >&2
    exit 1
fi
ROOT="$GNU_OUT/vk-gl-cts"
WORK="$GNU_OUT/work/vk-gl-cts"
rm -rf "$ROOT" "$WORK"
mkdir -p "$ROOT" "$WORK"
pins="$GNU/vk-gl-cts/sources.txt"
ins="-i $pins -i $GNU/vk-gl-cts/build_vk_gl_cts.sh"
# shellcheck disable=SC2013 # a name is one word
for name in $(awk '!/^#/ && NF == 4 { print $1 }' "$pins"); do
    ins="$ins -i $(asset "$name" "$pins")"
done
ref="$REPO/tests/graphics/debian-ref/debian-ref.sh"
sh "$ref" prepare -n
echo "gnu: vk-gl-cts: building in the builder guest; the log is $WORK/build.log"
# shellcheck disable=SC2086 # ins is words, and no path has a space
if ! sh "$ref" run -c "${GNU_VK_GL_CTS_CPUS:-16}" -m "${GNU_VK_GL_CTS_MIB:-24576}" -r "$WORK/guest.record" \
    $ins -o "$WORK/vk-gl-cts.tar" 'sh /tmp/build_vk_gl_cts.sh guest' > "$WORK/build.log" 2>&1; then
    tail -n 40 "$WORK/build.log" >&2
    echo "gnu: vk-gl-cts: the build in the builder guest failed; the log is $WORK/build.log" >&2
    exit 1
fi
tar -x -f "$WORK/vk-gl-cts.tar" -C "$ROOT"
rm -f "$WORK/vk-gl-cts.tar"
for v in $VERSIONS; do
    o="$ROOT/opt/vk-gl-cts-$v"
    [ -x "$o/vulkan/deqp-vk" ] && [ -x "$o/gl/glcts" ] && [ -s "$o/mustpass/vk-default.txt.gz" ] || {
        echo "gnu: vk-gl-cts: the stage of $v has no deqp-vk, glcts or case list" >&2
        exit 1
    }
done
grep '^vk-gl-cts: needs: ' "$WORK/build.log" | sed 's/^vk-gl-cts: /gnu: vk-gl-cts: /'
echo "gnu: vk-gl-cts: result: $(grep '^vk-gl-cts: result: ' "$WORK/build.log" | sed 's/^vk-gl-cts: result: //')"
