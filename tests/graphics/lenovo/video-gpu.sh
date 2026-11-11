#!/bin/sh
# video-gpu.sh: the checks of the GPU video step, run as root on Rach2 or
# Debian on the Lenovo's HD 4400, each the same on both systems.
#
#   sh video-gpu.sh make DIR
#   sh video-gpu.sh decode USER CLIPS OUT
#   sh video-gpu.sh hang USER CLIPS OUT
#   sh video-gpu.sh browsers USER CLIPS OUT
#
# USER is the ordinary account that runs the programs, a member of render
# and video, so that every batch passes the kernel's command parser; CLIPS
# holds the clips and OUT gets the evidence. VAHASH names vahash, built from
# vahash.c beside this script (./vahash in CLIPS by default). The checks:
#
#   make      the MPEG-2 and JPEG clips, 2 s of ffmpeg's testsrc2 pattern
#             at 1280x720, each encoded on one thread, with ffmpeg, which
#             only Debian has; the step copies them to Rach2.
#   decode    vainfo, then each clip of DECODE decoded by vahash in software
#             and through VA-API: DECODE/NAME.sw and NAME.va, a line a frame
#             with its MD5, and a line for each clip:
#               video-gpu: decode: NAME frames N va F, D differ from sw
#   hang      the H.264 1080p clip decoded through VA-API once alone and
#             then again and again while piglit's read-front tests, which
#             hung the render ring 8 times under Linux, run one after another
#             as root; the decode stops at the end of the loop after the last
#             trigger. hang/alone.va, hang/during.va, triggers.txt and
#             kernel.txt, and
#               video-gpu: hang: decode: L loops, F frames, B differ, status S
#               video-gpu: hang: kernel: H hangs, R resets of rcs0,
#                   V of vcs0, C of the chip, W wedged
#   browsers  each browser's video.sh on a headless Weston that renders with
#             the GPU, the six clips of 76-17a, three counted plays each,
#             with Firefox's FFmpegVideo log on to show VA-API; OUT/BROWSER
#             is video.sh's output, and its `video: result:` lines print.
#
# The status is 0 when the check ran to its end, whatever it found; the
# step judges the lines against Debian's. Nothing sets a time limit: a
# decode or trigger that never ends shows as a file that stops growing,
# which whoever runs the step watches.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
graphics=$(cd "$here/.." && pwd)
node=/dev/dri/renderD128

# The decode check's clips: 76-17a's H.264 clips, which clips.sha256 pins;
# the MPEG-2 and JPEG clips of `make`; and two of FFmpeg's VC-1 conformance
# clips, Advanced and Main (WMV3) profile, fetched from fate-suite.ffmpeg.org
# by the step and checked against the digests here.
DECODE="h264-720p.mp4 h264-1080p.mp4 mpeg2-720p.m2v jpeg-720p.mkv SA10091.vc1 SMM0005.rcv"
VC1_SUMS="c60a0da89873b40d7e436f1c29757c6cc71f3c51f2ac2ea8c6bd023762c94b6c  SA10091.vc1
2de6e6efe960bb0d93b0bfc0229e0b3b1fcdca72eb4b3e290de5f24fafa456f9  SMM0005.rcv"

# The browsers check's clips, in the reference's order.
PLAY="h264-720p.mp4 h264-1080p.mp4 vp9-720p.webm vp9-1080p.webm av1-720p.webm av1-1080p.webm"

usage()
{
    echo 'usage: video-gpu.sh make DIR | decode|hang|browsers USER CLIPS OUT' >&2
    exit 2
}

if [ "${1:-}" = make ]; then
    [ $# -eq 2 ] || usage
    mkdir -p "$2"
    src='testsrc2=size=1280x720:rate=30:duration=2,noise=alls=12:allf=t+u'
    ffmpeg -nostdin -loglevel error -y -threads 1 -f lavfi -i "$src" -c:v mpeg2video \
        -b:v 8M -g 15 -bf 2 -threads 1 -pix_fmt yuv420p -an -map_metadata -1 \
        -fflags +bitexact -flags:v +bitexact -f mpeg2video "$2/mpeg2-720p.m2v"
    ffmpeg -nostdin -loglevel error -y -threads 1 -f lavfi -i "$src" -c:v mjpeg -q:v 4 \
        -huffman default -threads 1 -pix_fmt yuvj420p -an -map_metadata -1 \
        -fflags +bitexact -flags:v +bitexact "$2/jpeg-720p.mkv"
    for f in mpeg2-720p.m2v jpeg-720p.mkv; do
        echo "video-gpu: made $f, $(wc -c < "$2/$f") bytes, sha256 $(sha256sum "$2/$f" | cut -c1-64)"
    done
    exit 0
fi

[ $# -eq 4 ] || usage
check=$1 user=$2 clips=$(cd "$3" && pwd) out=$4
case $check in decode | hang | browsers) ;; *) usage ;; esac
[ "$(id -u)" = 0 ] || { echo 'video-gpu: run as root' >&2; exit 2; }
vahash=${VAHASH:-$clips/vahash}
mkdir -p "$out"
out=$(cd "$out" && pwd)
chown "$user" "$out"

# asuser COMMAND: the shell command COMMAND as USER, in the runners'
# environment.
asuser()
{
    su -s /bin/sh "$user" -c "GRAPHICS_CLASS=conformance GRAPHICS_DONE=1; \
        export GRAPHICS_CLASS GRAPHICS_DONE; . '$graphics/guest-env.sh' && $1"
}

# differ A B: the frame lines of vahash's output B whose MD5 is not that of
# the same frame in A; a frame A lacks differs. B's loops each compare with
# A's first.
differ()
{
    awk 'NR == FNR { if ($1 == 0) want[$2] = $4; next }
        $1 ~ /^[0-9]+$/ && !($2 in want && want[$2] == $4) { n++ }
        END { print n + 0 }' "$1" "$2"
}

if [ "$check" = decode ]; then
    for f in $DECODE; do
        [ -f "$clips/$f" ] || { echo "video-gpu: no clip $clips/$f" >&2; exit 1; }
    done
    grep -E ' h264-(720|1080)p\.mp4$' "$graphics/runners/clips.sha256" |
        (cd "$clips" && sha256sum -c --quiet -) ||
        { echo 'video-gpu: the H.264 clips differ from clips.sha256' >&2; exit 1; }
    echo "$VC1_SUMS" | (cd "$clips" && sha256sum -c --quiet -) ||
        { echo 'video-gpu: the VC-1 clips differ from their digests' >&2; exit 1; }
    asuser "vainfo --display drm --device $node" > "$out/vainfo.txt" 2>&1 || true
    grep -E 'Driver version|VAEntrypointVLD' "$out/vainfo.txt" || true
    mkdir -p "$out/decode"
    chown "$user" "$out/decode"
    for f in $DECODE; do
        for m in sw va; do
            st=0
            asuser "'$vahash' $m '$clips/$f'" > "$out/decode/$f.$m" 2> "$out/decode/$f.$m.err" ||
                st=$?
            echo "status $st" >> "$out/decode/$f.$m.err"
        done
        n=$(grep -c '^0 ' "$out/decode/$f.sw" || true)
        v=$(grep -c '^0 ' "$out/decode/$f.va" || true)
        echo "video-gpu: decode: $f frames $n va $v, $(differ "$out/decode/$f.sw" "$out/decode/$f.va")" \
            "differ from sw; $(sed -n 's/^vahash: result: va .* \(decoder .*\)/\1/p' "$out/decode/$f.va")" \
            "$(grep -v -e '^status' -e 'reading ended' "$out/decode/$f.va.err" | tail -n 1)"
    done
    exit 0
fi

if [ "$check" = hang ]; then
    bin=${PIGLIT_BIN:-/usr/lib/x86_64-linux-gnu/piglit/bin}
    [ -x "$bin/read-front" ] || { echo "video-gpu: no $bin/read-front" >&2; exit 1; }
    h=$out/hang
    mkdir -p "$h"
    chown "$user" "$h"
    stop=$h/stop
    rm -f "$stop" "$h"/*.txt "$h"/*.out
    dec=
    trap ': > "$stop"; [ -z "$dec" ] || wait "$dec" 2> /dev/null || true; rm -f "$stop"' EXIT
    trap 'exit 1' HUP INT TERM

    klast=0
    # kernel_new: append the kernel's lines newer than the last seen, since
    # Rach2's log is a ring of 16 KiB.
    kernel_new()
    {
        dmesg | awk -v t0="$klast" '{
            t = $0; sub(/^\[ */, "", t); sub(/\].*/, "", t)
            if (t + 0 > t0 + 0) print
        }' >> "$h/kernel.txt"
        k=$(tail -n 1 "$h/kernel.txt" | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p')
        [ -z "$k" ] || klast=$k
    }
    k=$(dmesg | tail -n 1 | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p')
    [ -z "$k" ] || klast=$k
    : > "$h/kernel.txt"

    st=0
    asuser "'$vahash' va '$clips/h264-1080p.mp4'" > "$h/alone.va" 2> "$h/alone.err" || st=$?
    echo "video-gpu: hang: alone: $(grep -c '^0 ' "$h/alone.va" || true) frames, status $st"
    asuser "'$vahash' -s '$stop' va '$clips/h264-1080p.mp4'; echo \"status \$?\" >&2" \
        > "$h/during.va" 2> "$h/during.err" &
    dec=$!
    sleep 5
    n=0
    export PIGLIT_PLATFORM=gbm
    for first in clear-front-first ""; do
        for s in 2 4 6 8; do
            n=$((n + 1))
            t0=$(date +%s)
            st=0
            # shellcheck disable=SC2086 # the empty word is no argument
            "$bin/read-front" $first -samples=$s -auto > "$h/trigger-$n.out" 2>&1 || st=$?
            r=$(sed -n 's/^PIGLIT: *{"result": *"\([a-z]*\)".*/\1/p' "$h/trigger-$n.out" | tail -n 1)
            echo "$n ${r:-none} $st $t0 $(date +%s) read-front $first -samples=$s" >> "$h/triggers.txt"
            kernel_new
        done
    done
    : > "$stop"
    wait "$dec" || true
    dec=
    kernel_new
    count() { grep -cE "$1" "$h/kernel.txt" || true; }
    loops=$(grep -c '^vahash: loop ' "$h/during.err" || true)
    echo "video-gpu: hang: triggers: $n run, $(awk '$2 == "fail" && $3 == 1' "$h/triggers.txt" | wc -l)" \
        "failed with status 1, from $(head -n 1 "$h/triggers.txt" | cut -d' ' -f4)" \
        "to $(tail -n 1 "$h/triggers.txt" | cut -d' ' -f5)"
    echo "video-gpu: hang: decode: $loops loops, $(grep -c '^[0-9]' "$h/during.va" || true) frames," \
        "$(differ "$h/alone.va" "$h/during.va") differ, $(grep '^status' "$h/during.err" | tail -n 1)," \
        "last loop ended at $(sed -n 's/^vahash: loop .* ended at //p' "$h/during.err" | tail -n 1)"
    echo "video-gpu: hang: kernel: $(count 'GPU HANG') hangs," \
        "$(count '(Resetting|reset:) rcs0 for ') resets of rcs0," \
        "$(count '(Resetting|reset:) vcs0 for ') of vcs0," \
        "$(count 'Resetting chip for |reset: the chip for ') of the chip," \
        "$(count '[Ww]edged') wedged"
    exit 0
fi

# browsers
for f in $PLAY; do
    [ -f "$clips/$f" ] || { echo "video-gpu: no clip $clips/$f" >&2; exit 1; }
done
for b in chromium firefox; do
    st=0
    # shellcheck disable=SC2086 # the clips' names
    asuser "RUNNER_RENDERER=gl VIDEO_MOZ_LOG=PlatformDecoderModule:5,FFmpegVideo:4; \
        export RUNNER_RENDERER VIDEO_MOZ_LOG; \
        sh '$graphics/runners/video.sh' -b $b -c '$clips' -o '$out/$b' $PLAY" \
        > "$out/$b.log" 2>&1 || st=$?
    grep '^video: result:' "$out/$b.log" || true
    echo "video-gpu: browsers: $b status $st"
done
exit 0
