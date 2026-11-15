#!/bin/sh
# gpuvideo.sh: video on the Lenovo's GPU judged against Linux's on the same
# machine, as the native cases video/* run 78-14b's checks.
#
#   gpuvideo.sh decode|browsers|hang SUMS LINUX [SPREAD]
#
# Run as root by etude-guest in the case's workspace, in a mount namespace of
# the case's own, on the GNU root whose desktop account is in render and
# video. The workspace holds tests/graphics/lenovo/video-gpu.sh, guest-env.sh
# and the runners it uses. The clips and vahash, which Debian's FFmpeg
# headers build, are not in the tree: the Debian boot that took Linux's
# reference left them on the data partition, /dev/disk0p6, in
# native/video/clips, and SUMS, tests/graphics/ref/lenovo-video/data.sha256,
# has the SHA-256 of each. LINUX, tests/graphics/ref/lenovo-video/linux.tar.gz,
# is that boot's output of the same checks: linux/ and linux-*.out. SPREAD,
# tests/graphics/ref/lenovo-video/spread, is the frames by which the browsers
# check lets a clip's median of dropped frames exceed Linux's most, by
# browser.
#
# The script mounts the partition read-only, copies the clips into the
# workspace, checks each against SUMS and unmounts it, lays out the kit as
# video-gpu.sh expects, runs `video-gpu.sh CHECK desktop CLIPS rach2`, and
# judges as 78-14b's step does:
#
#   decode    each clip's frames, in software and through VA-API, the same
#             MD5s line for line as Linux's
#   browsers  `judge.py video` for each browser and clip against Linux's
#             plays and decoders, under runners/video.limits and SPREAD
#   hang      the decode during the triggers with 0 frames differing from
#             the decode alone and status 0; the kernel's hangs and resets of
#             rcs0 as Linux's, and no reset of vcs0 or of the chip and
#             nothing wedged
#
# The last line is `gpuvideo: result: CHECK: pass` or `fail: why`.
set -eu

[ $# -eq 3 ] || [ $# -eq 4 ] || {
    echo "usage: gpuvideo.sh decode|browsers|hang SUMS LINUX [SPREAD]" >&2
    exit 2
}
check=$1
here=$(pwd)
sums=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
linux=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
spread=''
[ $# -lt 4 ] || spread=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
data=/dev/disk0p6
mounted=''
DECODE="h264-720p.mp4 h264-1080p.mp4 mpeg2-720p.m2v jpeg-720p.mkv SA10091.vc1 SMM0005.rcv"
PLAY="h264-720p.mp4 h264-1080p.mp4 vp9-720p.webm vp9-1080p.webm av1-720p.webm av1-1080p.webm"

fail()
{
    echo "gpuvideo: result: $check: fail: $*"
    exit 1
}

# shellcheck disable=SC2329 # the exit trap runs it
cleanup()
{
    [ -z "$mounted" ] || umount "$mounted" 2> /dev/null || :
    if command -v mixer > /dev/null; then mixer vol 1 > /dev/null || :; fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case $check in
    decode | browsers | hang) ;;
    *) fail "no check $check" ;;
esac
[ -f "$sums" ] && [ -f "$linux" ] || fail "no $2 or $3"
[ -z "$spread" ] || [ -f "$spread" ] || fail "no $4"

# The clips, from the data partition, each as SUMS has it.
rm -rf clips data kit rach2 ref
mkdir -p data clips
mount -t ext2 -o ro "$data" data || fail "$data would not mount read-only"
mounted=$here/data
[ -d data/native/video/clips ] || fail "no native/video/clips on $data"
cp -R data/native/video/clips/. clips/
umount data
mounted=''
(cd clips && grep -v '^#' "$sums" | sha256sum -c) > clips.check 2>&1 || {
    cat clips.check
    fail "a clip differs from $2"
}
chmod 755 clips/vahash

# The kit as video-gpu.sh finds it: its own directory beside the runners.
mkdir -p kit/lenovo kit/runners
cp video-gpu.sh kit/lenovo/
cp guest-env.sh kit/
for f in video.sh common.sh server.py cdp.py events.py video.html clips.sha256 judge.py video.limits; do
    cp "$f" kit/runners/
done
chmod -R a+rX kit clips
if command -v mixer > /dev/null; then mixer vol 1 > /dev/null || :; fi

status=0
sh kit/lenovo/video-gpu.sh "$check" desktop "$here/clips" "$here/rach2" > rach2.out 2>&1 || status=$?
cat rach2.out
[ "$status" = 0 ] || fail "video-gpu.sh $check exited $status"
mkdir ref
tar -xzf "$linux" -C ref

bad=''
case $check in
    decode)
        for f in $DECODE; do
            for m in sw va; do
                grep -v '^vahash' "ref/linux/decode/$f.$m" > a 2> /dev/null || : > a
                grep -v '^vahash' "rach2/decode/$f.$m" > b 2> /dev/null || : > b
                if [ -s a ] && cmp -s a b; then
                    echo "gpuvideo: $f $m: as Linux's"
                else
                    echo "gpuvideo: $f $m: differs"
                    bad="$bad $f.$m"
                fi
            done
        done
        ;;
    browsers)
        for b in chromium firefox; do
            for c in $PLAY; do
                python3 -I kit/runners/judge.py video "ref/linux/$b/$b-$c.plays" "rach2/$b/$b-$c.plays" \
                    "ref/linux/$b/$b-$c.decoders" "rach2/$b/$b-$c.decoders" kit/runners/video.limits ${spread:+"$spread"} ||
                    bad="$bad $b-$c"
            done
        done
        ;;
    hang)
        grep -h '^video-gpu: hang:' ref/linux-hang.out | sed 's/^/gpuvideo: linux: /'
        dec=$(grep '^video-gpu: hang: decode:' rach2.out | tail -n 1)
        case $dec in
            *' 0 differ, status 0,'*) ;;
            *) bad="$bad decode" ;;
        esac
        # The counts of the kernel's line: hangs, rcs0, vcs0, chip, wedged.
        counts() { sed -n 's/^video-gpu: hang: kernel: \([0-9]*\) hangs, \([0-9]*\) resets of rcs0, \([0-9]*\) of vcs0, \([0-9]*\) of the chip, \([0-9]*\) wedged$/\1 \2 \3 \4 \5/p' "$1" | tail -n 1; }
        want=$(counts ref/linux-hang.out)
        got=$(counts rach2.out)
        [ -n "$want" ] || fail "Linux's hang line is missing"
        # shellcheck disable=SC2086 # five counts
        set -- $want
        lh=$1 lr=$2
        # shellcheck disable=SC2086 # five counts
        set -- ${got:-x x x x x}
        [ "$1" = "$lh" ] && [ "$2" = "$lr" ] || bad="$bad hangs-and-resets"
        [ "$3" = 0 ] && [ "$4" = 0 ] && [ "$5" = 0 ] || bad="$bad vcs0-chip-wedged"
        ;;
esac
[ -z "$bad" ] || fail "not as Linux's:$bad"
echo "gpuvideo: result: $check: pass"
