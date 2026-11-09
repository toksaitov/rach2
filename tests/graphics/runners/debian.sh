#!/bin/sh
# debian.sh: run a browser runner in the Debian reference guest and keep
# what it writes, as the references under tests/graphics/ref/debian are made.
#
#   debian.sh webgl [-p PROFILE] [-c CPUS] [-m MIB] OUT [WEBGL-ARG]...
#   debian.sh video [-p PROFILE] [-c CPUS] [-m MIB] OUT [VIDEO-ARG]...
#   debian.sh clips DIR
#
# `webgl` and `video` boot the guest with debian-ref.sh run in the display
# PROFILE (virtio-vga) with CPUS processors (4) and MIB of memory (4096), as
# a Done's run with an empty shader cache, copy in this directory and, for
# webgl, the pinned WebGL suite, run webgl.sh or video.sh with the ARGs and
# -o, and put the runner's output in OUT: the record, which gets the guest
# machine's lines from debian-ref.sh and the host's from `common.sh host`
# after the runner's own, the pages or plays files, `results`, the runner's
# lines, and `status`, its exit status. video's clips come from DIR of
# `clips`, by default build/cache/graphics-clips of the main clone.
# `clips` makes the clips that video.sh names in the guest with Debian's
# ffmpeg, which it installs from the snapshot for that boot alone, so that
# the reference guest's browsers find no codec library that Rach2's root
# lacks, and checks them against clips.sha256.
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
tar -czf "$work/runners.tar.gz" -C "$here/.." runners

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

# run KIND PROFILE CPUS MIB OUT ARG...: the runner KIND in the guest.
run()
{
    kind=$1 profile=$2 cpus=$3 mib=$4 out=$5
    shift 5
    ins="-i $work/runners.tar.gz"
    prep='tar -xzf runners.tar.gz'
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
    esac
    mkdir -p "$out"
    # shellcheck disable=SC2086 # ins are words
    sh "$ref" run -p "$profile" -c "$cpus" -m "$mib" -g conformance -d -r "$work/machine" \
        $ins -o "$work/out.tar.gz" \
        "cd /tmp && $prep && mkdir -p /tmp/out && { sh runners/$kind.sh -o /tmp/out $*; echo \$? > /tmp/out/status; } &&
         tar -czf /tmp/out.tar.gz -C /tmp/out ." || die "the guest's run failed"
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
    webgl|video)
        profile=virtio-vga cpus=4 mib=4096
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
        run "$cmd" "$profile" "$cpus" "$mib" "$out" "$@"
        ;;
    clips)
        dir=${1:-$clips}
        mkdir -p "$dir"
        sh "$ref" run -c 8 -m 4096 -i "$work/runners.tar.gz" -o "$work/clips.tar" \
            "cd /tmp && tar -xzf runners.tar.gz &&
             sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install ffmpeg > /tmp/apt.log 2>&1 &&
             sh runners/video.sh make /tmp/clips && tar -cf /tmp/clips.tar -C /tmp/clips ." ||
            die "the clips could not be made"
        tar -xf "$work/clips.tar" -C "$dir"
        (cd "$dir" && sha256sum -c --quiet "$here/clips.sha256") ||
            die "the clips made differ from clips.sha256"
        echo "debian: result: clips in $dir"
        ;;
    *)
        echo "usage: debian.sh webgl|video [-p PROFILE] [-c CPUS] [-m MIB] OUT [ARG]... | clips [DIR]" >&2
        exit 2
        ;;
esac
