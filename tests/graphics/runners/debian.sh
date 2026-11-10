#!/bin/sh
# debian.sh: run a graphics runner in the Debian reference guest and keep
# what it writes, as the references under tests/graphics/ref/debian are made.
#
#   debian.sh webgl [-p PROFILE] [-c CPUS] [-m MIB] OUT [WEBGL-ARG]...
#   debian.sh video [-p PROFILE] [-c CPUS] [-m MIB] OUT [VIDEO-ARG]...
#   debian.sh igt [-p PROFILE] [-c CPUS] [-m MIB] OUT [IGT-ARG]...
#   debian.sh modetest [-p PROFILE] [-c CPUS] [-m MIB] OUT [MODETEST-ARG]...
#   debian.sh piglit [-p PROFILE] [-c CPUS] [-m MIB] OUT [PIGLIT-ARG]...
#   debian.sh glmark2 [-p PROFILE] [-c CPUS] [-m MIB] OUT [GLMARK2-ARG]...
#   debian.sh deqp [-p PROFILE] [-c CPUS] [-m MIB] OUT [DEQP-ARG]...
#   debian.sh vkcube [-p PROFILE] [-c CPUS] [-m MIB] OUT [VKCUBE-ARG]...
#   debian.sh clips [-t TAR] [DIR]
#
# A runner's command boots the guest with debian-ref.sh run in the display
# PROFILE (virtio-vga) with CPUS processors (4) and MIB of memory (8192), as
# a Done's run with an empty shader cache, copies in this directory,
# igt-76.list and the dEQP lists of tests/graphics, runs the runner with the
# ARGs and -o, and puts the runner's output in OUT: the record, which gets
# the guest machine's lines from debian-ref.sh and the host's from `common.sh
# host` after the runner's own, the runner's files, `results`, the runner's
# lines, and `status`, its exit status. The runners of a conformance class
# (webgl, video, igt, modetest, piglit and deqp) run with
# GRAPHICS_CLASS=conformance and glmark2 and vkcube with performance. Each
# copies in what its runner needs: webgl the pinned WebGL suite; video the clips
# of DIR of `clips`, by default build/cache/graphics-clips of the main clone;
# igt the IGT stage of this tree, which it builds with `make ARCH=x86_64
# gnu-igt` when it is missing, unpacked over the guest's /usr, with
# intel-gpu-tools installed for the libraries it links and Linux's sw_sync
# loaded by ../debian-ref/sw_sync.sh, which Debian's kernel lacks; modetest
# drmcapture, built there from its source; piglit piglit, installed from the
# snapshot; deqp the VK-GL-CTS release that its -v names (1.4.7.1 when none),
# from this tree's GNU build, which it builds with `make ARCH=x86_64
# gnu-vk-gl-cts` when it is missing, unpacked under /opt as the root has it.
# igt, modetest and vkcube run their runner as root, with vkms loaded in the
# profile `none`, and the others as the guest's user; vkcube builds its layer
# with the guest's GCC. All of them run in /tmp, where the lists are.
# `clips` makes the clips that video.sh names in DIR, unless DIR holds them
# already, in the guest with Debian's ffmpeg, which it installs from the
# snapshot for that boot alone, so that the reference guest's browsers find
# no codec library that Rach2's root lacks, checks them against
# clips.sha256, and with -t writes them as the archive TAR, which the video
# cases put on Rach2. Each command prepares the guest first when it has no
# prepared image of its current inputs. A package a command installs comes
# from the snapshot for that boot alone, so that the prepared image, which
# every worktree shares, does not change.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
common=$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$tree/.git")
main=$(dirname "$common")
ref=$tree/tests/graphics/debian-ref/debian-ref.sh

die()
{
    echo "debian: $*" >&2
    exit 1
}

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "${TMPDIR:-$tree/build/tmp}"
work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/debian-runner.XXXXXX")
tar -czf "$work/runners.tar.gz" -C "$here/.." runners igt-76.list deqp-vk-lavapipe.txt.gz \
    deqp-vk-lavapipe.flakes deqp-vk-venus-smoke.txt

# suite: the path of the pinned WebGL archive, fetched into the main
# clone's cache as the GNU layer's step fetches it.
suite()
{
    (
        # shellcheck disable=SC2034 # fetch_asset.sh reads REPO
        GNU=$tree/vendor/gnu REPO=$main
        # shellcheck source=../../../vendor/gnu/debian/fetch_asset.sh
        . "$GNU/debian/fetch_asset.sh"
        asset webgl
    )
}

# install PACKAGE...: the guest's command that installs PACKAGEs from the
# snapshot, its log in /tmp/apt.log.
install()
{
    echo "sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install $* >> /tmp/apt.log 2>&1"
}

# run KIND PROFILE CPUS MIB OUT ARG...: the runner KIND in the guest.
run()
{
    kind=$1 profile=$2 cpus=$3 mib=$4 out=$5
    shift 5
    ins="-i $work/runners.tar.gz"
    prep='tar -xzf runners.tar.gz'
    class=conformance as=
    [ "$profile" != none ] || prep="$prep && sudo modprobe vkms"
    case $kind in
        webgl)
            archive=$(suite) || die "the WebGL suite could not be fetched"
            ins="$ins -i $archive"
            prep="$prep && mkdir webgl && tar -xzf $(basename "$archive") -C webgl --strip-components=3 \
                --wildcards '*/sdk/tests/*' && rm $(basename "$archive")"
            set -- -s /tmp/webgl "$@"
            ;;
        video)
            [ -d "$clips" ] || die "no clips in $clips; run debian.sh clips"
            tar -cf "$work/clips.tar" -C "$clips" .
            ins="$ins -i $work/clips.tar"
            prep="$prep && mkdir clips && tar -xf clips.tar -C clips && rm clips.tar"
            set -- -c /tmp/clips "$@"
            ;;
        igt)
            stage=$tree/build/x86_64/vendor/gnu/igt
            [ -x "$stage/usr/bin/igt_runner" ] || make -s -C "$tree" ARCH=x86_64 gnu-igt ||
                die "the IGT stage could not be built"
            tar -czf "$work/igt.tar.gz" -C "$stage" usr
            ins="$ins -i $work/igt.tar.gz -i $tree/tests/graphics/debian-ref/sw_sync.sh"
            prep="$prep && sudo tar -xzf igt.tar.gz -C / && rm igt.tar.gz && $(install intel-gpu-tools libjson-c5)"
            prep="$prep && sudo sh sw_sync.sh"
            as='sudo -E'
            ;;
        modetest)
            ins="$ins -i $tree/tests/graphics/capture/drmcapture.c"
            prep="$prep && cc -std=c99 -O2 -o /tmp/drmcapture /tmp/drmcapture.c"
            set -- -c /tmp/drmcapture "$@"
            as='sudo -E'
            ;;
        piglit)
            prep="$prep && $(install piglit)"
            ;;
        glmark2)
            class=performance
            ;;
        vkcube)
            class=performance
            as='sudo -E'
            ;;
        deqp)
            release=1.4.7.1 last=
            for a in "$@"; do
                [ "$last" != -v ] || release=$a
                last=$a
            done
            cts=$tree/build/x86_64/vendor/gnu/vk-gl-cts/opt
            [ -x "$cts/vk-gl-cts-$release/vulkan/deqp-vk" ] ||
                make -s -C "$tree" ARCH=x86_64 gnu-vk-gl-cts || die "VK-GL-CTS could not be built"
            [ -d "$cts/vk-gl-cts-$release" ] || die "no VK-GL-CTS $release in $cts"
            tar -czf "$work/cts.tar.gz" -C "$cts" "vk-gl-cts-$release"
            ins="$ins -i $work/cts.tar.gz"
            prep="$prep && sudo tar -xzf cts.tar.gz -C /opt && rm cts.tar.gz"
            ;;
    esac
    mkdir -p "$out"
    # shellcheck disable=SC2086 # ins are words
    sh "$ref" run -p "$profile" -c "$cpus" -m "$mib" -g "$class" -d -r "$work/machine" \
        $ins -o "$work/out.tar.gz" \
        "cd /tmp && $prep && mkdir -p /tmp/out && { $as sh runners/$kind.sh -o /tmp/out $*; echo \$? > /tmp/out/status; } &&
         sudo tar -czf /tmp/out.tar.gz -C /tmp/out . && sudo chown gfx /tmp/out.tar.gz" || die "the guest's run failed"
    tar -xzf "$work/out.tar.gz" -C "$out"
    {
        sed 's/^/guest /' "$work/machine"
        sh "$here/common.sh" host
    } >> "$out/record"
    echo "debian: result: $kind status $(cat "$out/status") in $out"
}

clips=$main/build/cache/graphics-clips
cmd=${1:-}
[ $# -gt 0 ] && shift
case $cmd in
    webgl|video|igt|modetest|piglit|glmark2|deqp|vkcube)
        profile=virtio-vga cpus=4 mib=8192
        while getopts p:c:m: o; do
            case $o in
                p) profile=$OPTARG ;;
                c) cpus=$OPTARG ;;
                m) mib=$OPTARG ;;
                *) exit 2 ;;
            esac
        done
        shift $((OPTIND - 1))
        [ $# -ge 1 ] || die "usage: debian.sh $cmd [-p PROFILE] [-c CPUS] [-m MIB] OUT [ARG]..."
        out=$1
        shift
        sh "$ref" prepare -n
        run "$cmd" "$profile" "$cpus" "$mib" "$out" "$@"
        ;;
    clips)
        tar=
        while getopts t: o; do
            case $o in
                t) tar=$OPTARG ;;
                *) exit 2 ;;
            esac
        done
        shift $((OPTIND - 1))
        dir=${1:-$clips}
        mkdir -p "$dir"
        if (cd "$dir" && sha256sum -c --quiet "$here/clips.sha256") > /dev/null 2>&1; then
            # shellcheck disable=SC2046 # the clips' names are words
            [ -z "$tar" ] || tar -cf "$tar" -C "$dir" $(awk '!/^#/ { print $2 }' "$here/clips.sha256")
            echo "debian: result: clips in $dir"
            exit 0
        fi
        sh "$ref" prepare -n
        sh "$ref" run -c 8 -m 4096 -i "$work/runners.tar.gz" -o "$work/clips.tar" \
            "cd /tmp && tar -xzf runners.tar.gz &&
             sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install ffmpeg > /tmp/apt.log 2>&1 &&
             sh runners/video.sh make /tmp/clips && tar -cf /tmp/clips.tar -C /tmp/clips ." ||
            die "the clips could not be made"
        tar -xf "$work/clips.tar" -C "$dir"
        (cd "$dir" && sha256sum -c --quiet "$here/clips.sha256") ||
            die "the clips made differ from clips.sha256"
        # shellcheck disable=SC2046 # the clips' names are words
        [ -z "$tar" ] || tar -cf "$tar" -C "$dir" $(awk '!/^#/ { print $2 }' "$here/clips.sha256")
        echo "debian: result: clips in $dir"
        ;;
    *)
        echo "usage: debian.sh webgl|video|igt|modetest|piglit|glmark2|deqp|vkcube [-p PROFILE] [-c CPUS] [-m MIB] OUT [ARG]... | clips [-t TAR] [DIR]" >&2
        exit 2
        ;;
esac
