#!/bin/sh
# Run one new browser acceptance case in the pinned Debian reference guest.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
[ "$#" -eq 3 ] || { echo 'usage: debian.sh BROWSER CASE OUT' >&2; exit 2; }
browser=$1 mode=$2 out=$3
case $browser in chromium|firefox) ;; *) exit 2 ;; esac
case $mode in pages|sandbox|webgpu|recovery|sound|webgl|video-*) ;; *) exit 2 ;; esac
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$tree/build/tmp" "$out"
work=$(mktemp -d "$tree/build/tmp/browser-ref.XXXXXX")
ins=
prep=
if [ "$mode" = webgpu ]; then
    make -C "$tree" build/tests/cases/browser/files.tar.gz
    cp "$tree/build/tests/cases/browser/files.tar.gz" "$work/browser.tar.gz"
else
    tar -czf "$work/browser.tar.gz" -C "$here/.." browser runners guest-env.sh ref/debian
fi
case $mode in
    video-*)
        make -C "$tree" build/tests/cases/graphics/clips.tar
        ins="-i $tree/build/tests/cases/graphics/clips.tar"
        prep='mkdir clips && tar -xf clips.tar -C clips &&'
        ;;
    webgl)
        # The reference helper stages the pinned WebGL suite for this case.
        exec sh "$here/../runners/debian.sh" webgl "$out" -b "$browser" -v 2.0.1 00
        ;;
esac
command="sh browser/run.sh $browser $mode /tmp/browser-out"
if [ "$mode" = sandbox ] && [ "$browser" = chromium ]; then
    command="sudo python3 browser/sandbox.py gfx $browser /tmp/browser-out"
elif [ "$mode" = sound ]; then
    command="sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install pulseaudio pulseaudio-utils > /tmp/browser-out/apt.log 2>&1 && python3 browser/sound.py pulse $browser /tmp/browser-out"
fi
ref=$here/../debian-ref/debian-ref.sh
sh "$ref" prepare -n
# shellcheck disable=SC2086 # ins contains a validated prepared artifact path
sh "$ref" run -p virtio-vga -c 4 -m 8192 -g conformance -d -r "$out/machine" \
    -i "$work/browser.tar.gz" $ins -o "$work/result.tar.gz" \
    "cd /tmp && tar -xzf browser.tar.gz && $prep mkdir browser-out &&
     { $command; echo \$? > browser-out/status; };
     tar -czf /tmp/result.tar.gz -C /tmp/browser-out ."
tar -xzf "$work/result.tar.gz" -C "$out"
sh "$here/../runners/common.sh" host >> "$out/record"
cat "$out/status"
[ "$(cat "$out/status")" = 0 ]
