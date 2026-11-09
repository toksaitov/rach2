#!/bin/sh
# video.sh: play video clips in a browser inside a graphics guest, Rach2's or
# Debian's, and count the frames it showed and dropped.
#
#   video.sh [-b BROWSER] [-c CLIPS] [-w WARMUP] [-r REPS] [-o OUT] CLIP...
#   video.sh make DIR
#
# Plays each CLIP, a name of the table below found in CLIPS
# (/usr/share/graphics-clips) and checked against clips.sha256, muted and
# fullscreen in BROWSER (chromium; or firefox) under a headless Weston,
# WARMUP times (1) and then REPS times (3), each play to the clip's end in a
# new video element, through video.html; each clip has a new browser. The
# warm-up is played and kept but not counted: browser numbers in QEMU vary
# between runs, and the repetitions show by how much.
#
# The decoder is named by the browser: Chromium's by the kVideoDecoderName
# that its DevTools protocol's Media domain reports (cdp.py), Firefox's by
# the description its PlatformDecoderModule log gives as the decoder is
# made, such as "ffmpeg video decoder (RDD remote)"; a space in the name is
# written `_` in the result line. OUT (.) gets the record and, for each clip,
# BROWSER-CLIP.plays, one line a play, `REP WARM TOTAL DROPPED CORRUPTED
# SECONDS WIDTHxHEIGHT ERROR`, and BROWSER-CLIP.decoders. Each clip prints
# one line, the counts of the counted plays:
#
#   video: result: BROWSER CLIP codec C size WxH frames N,N,N
#       dropped D,D,D corrupted X,X,X decoder NAME errors E
#
# A browser that posts nothing for 120 s has stopped and is ended; its clip
# then has fewer plays. The status is 0 when every play of every clip ran
# without a media error, and 1 otherwise.
#
# `make DIR` makes the clips with ffmpeg, as debian.sh clips does in the
# Debian guest: 10 s at 30 frames a second of ffmpeg's testsrc2 pattern
# with temporal noise, so that every frame costs its decoder about what a
# filmed one would, each encoded on one thread, so that the same ffmpeg makes
# the same bytes.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)

# clip NAME: set codec, width, height and the encoder's words for NAME.
clip()
{
    case $1 in
        h264-720p.mp4) codec=h264 width=1280 height=720 bitrate=4M ;;
        h264-1080p.mp4) codec=h264 width=1920 height=1080 bitrate=8M ;;
        vp9-720p.webm) codec=vp9 width=1280 height=720 bitrate=3M ;;
        vp9-1080p.webm) codec=vp9 width=1920 height=1080 bitrate=6M ;;
        av1-720p.webm) codec=av1 width=1280 height=720 bitrate=2M ;;
        av1-1080p.webm) codec=av1 width=1920 height=1080 bitrate=4M ;;
        *) return 1 ;;
    esac
    case $codec in
        h264) encoder="-c:v libx264 -preset medium -profile:v high -b:v $bitrate -maxrate $bitrate -bufsize $bitrate -movflags +faststart" ;;
        vp9) encoder="-c:v libvpx-vp9 -b:v $bitrate -deadline realtime -cpu-used 8 -row-mt 0" ;;
        av1) encoder="-c:v libaom-av1 -usage realtime -cpu-used 8 -b:v $bitrate -row-mt 0 -tiles 1x1" ;;
    esac
}

CLIPS="h264-720p.mp4 h264-1080p.mp4 vp9-720p.webm vp9-1080p.webm av1-720p.webm av1-1080p.webm"

if [ "${1:-}" = make ]; then
    [ $# -eq 2 ] || { echo "usage: video.sh make DIR" >&2; exit 2; }
    mkdir -p "$2"
    for name in $CLIPS; do
        clip "$name"
        # shellcheck disable=SC2086 # the encoder's words
        ffmpeg -nostdin -loglevel error -y -threads 1 -f lavfi \
            -i "testsrc2=size=${width}x$height:rate=30:duration=10,noise=alls=12:allf=t+u" \
            $encoder -threads 1 -pix_fmt yuv420p -g 60 -an -map_metadata -1 \
            -fflags +bitexact -flags:v +bitexact "$2/$name"
        echo "video: made $name, $(wc -c < "$2/$name") bytes"
    done
    exit 0
fi

# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

browser=chromium clips=/usr/share/graphics-clips warmup=1 reps=3 out=.
silence=120
while getopts b:c:w:r:o: o; do
    case $o in
        b) browser=$OPTARG ;;
        c) clips=$OPTARG ;;
        w) warmup=$OPTARG ;;
        r) reps=$OPTARG ;;
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { echo "usage: video.sh [-b BROWSER] [-c CLIPS] [-w WARMUP] [-r REPS] [-o OUT] CLIP..." >&2; exit 2; }
case $browser in chromium|firefox) ;; *) echo "video: no browser $browser" >&2; exit 2 ;; esac
for name; do
    clip "$name" || { echo "video: no clip $name; one of $CLIPS" >&2; exit 2; }
    [ -f "$clips/$name" ] || { echo "video: no clip $clips/$name" >&2; exit 1; }
    grep " $name\$" "$RUNNER_DIR/clips.sha256" | (cd "$clips" && sha256sum -c --quiet -) ||
        { echo "video: $clips/$name differs from clips.sha256" >&2; exit 1; }
done

runner_init video "$out"
runner_start
runner_record "case video $browser $*"
runner_record "warmup $warmup"
runner_record "repetitions $reps"
runner_record "clips $clips"
runner_browser_record "$browser"
runner_weston
runner_server "$clips" "video-runner.html=$RUNNER_DIR/video.html"
events=$RUNNER_WORK/events
lines=0
status=0
for name; do
    clip "$name"
    sum=$(awk -v n="$name" '$2 == n { print $1 }' "$RUNNER_DIR/clips.sha256")
    runner_record "clip $name $codec ${width}x$height 30fps 10s $(wc -c < "$clips/$name") bytes sha256 $sum"
    log=$RUNNER_OUT/$browser-$name.log
    : > "$log"
    # shellcheck disable=SC2034 # runner_browser reads them
    RUNNER_CDP='' RUNNER_MOZ_LOG=''
    # shellcheck disable=SC2034 # runner_browser reads them
    case $browser in
        chromium) RUNNER_CDP=$RUNNER_WORK/cdp; : > "$RUNNER_CDP" ;;
        firefox) RUNNER_MOZ_LOG=PlatformDecoderModule:5 ;;
    esac
    runner_browser "$browser" "http://127.0.0.1:$RUNNER_PORT/video-runner.html?clip=$name&warm=$warmup&reps=$reps" "$log"
    waited=$(python3 -I "$RUNNER_DIR/events.py" wait "$events" "$lines" "$RUNNER_BROWSER" "$silence")
    state=${waited% *} next=${waited#* }
    runner_stop
    [ "$lines" -gt 0 ] || python3 -I "$RUNNER_DIR/events.py" renderer "$events" >> "$RUNNER_OUT/record"
    plays=$RUNNER_OUT/$browser-$name.plays
    python3 -I "$RUNNER_DIR/events.py" plays "$events" "$lines" "$next" > "$plays"
    lines=$next
    case $browser in
        chromium)
            python3 -I -c '
import json, sys
names = []
for line in open(sys.argv[1], encoding="utf-8"):
    e = json.loads(line)
    for p in e["params"].get("properties", []):
        if p.get("name") in ("kVideoDecoderName", "kIsPlatformVideoDecoder"):
            names.append("%s=%s" % (p["name"], p["value"]))
print("\n".join(dict.fromkeys(names)))' "$RUNNER_CDP" > "$RUNNER_OUT/$browser-$name.decoders"
            decoder=$(sed -n 's/^kVideoDecoderName=//p' "$RUNNER_OUT/$browser-$name.decoders" | tail -n 1)
            ;;
        firefox)
            {
                grep -a -o 'has been initialized - description: .*' "$log" | sed 's/^.*description: //'
                grep -a -o 'PDMInitializer, [0-9]*: .*' "$log" | sort -u
            } > "$RUNNER_OUT/$browser-$name.decoders" || true
            decoder=$(sed -n '1s/, process: .*//p' "$RUNNER_OUT/$browser-$name.decoders")
            ;;
    esac
    errors=$(awk '$2 == 0 && $8 != "none"' "$plays" | wc -l)
    counted=$(awk '$2 == 0' "$plays" | wc -l)
    [ "$errors" -eq 0 ] && [ "$counted" -eq "$reps" ] || status=1
    awk -v b="$browser" -v n="$name" -v c="$codec" -v d="$(echo "${decoder:-unknown}" | tr ' ' _)" -v e="$errors" -v s="$state" '
        $2 == 0 { f = f sep $3; x = x sep $4; k = k sep $5; z = $7; sep = "," }
        END {
            printf "video: result: %s %s codec %s size %s frames %s dropped %s corrupted %s decoder %s errors %d browser %s\n",
                b, n, c, z ? z : "none", f ? f : "none", x ? x : "none", k ? k : "none", d, e, s
        }' "$plays" | tee -a "$RUNNER_OUT/results"
done
runner_finish
exit "$status"
