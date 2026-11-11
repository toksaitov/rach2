#!/bin/sh
# Run one browser acceptance case inside Rach2 or the Debian reference guest.
set -eu
browser=$1 mode=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)
case $mode in
    webgl)
        exec sh "$here/../runners/webgl.sh" -b "$browser" -v 2.0.1 -o "$out" 00
        ;;
    video-*)
        clip=${mode#video-}
        case $clip in h264-*) clip=$clip.mp4 ;; *) clip=$clip.webm ;; esac
        exec sh "$here/../runners/video.sh" -b "$browser" -c "$here/../clips" -o "$out" -t "$clip"
        ;;
esac
RUNNER_DIR=$(cd "$here/../runners" && pwd)
. "$RUNNER_DIR/common.sh"
# shellcheck disable=SC2034 # common.sh reads the runner state
RUNNER_NAME=browser RUNNER_OUT=$out RUNNER_WORK=''
# shellcheck disable=SC2034 # common.sh reads the process IDs
RUNNER_BROWSER='' RUNNER_SERVER='' RUNNER_WESTON=''
trap runner_cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out"
RUNNER_WORK=$(mktemp -d "${TMPDIR:-/tmp}/browser.XXXXXX")
if [ "$mode" = webgpu-hasvk ]; then
    # The caller's DRM Weston drives the Lenovo's panel at its 1366x768 mode,
    # which the record's size must name.
    # shellcheck disable=SC2034 # runner_start records them
    RUNNER_WIDTH=1366 RUNNER_HEIGHT=768
fi
runner_start
runner_record "case $browser $mode"
case $mode in
    webgpu-venus|webgpu-venus-native)
        VK_DRIVER_FILES=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
        export VK_DRIVER_FILES
        runner_record "icd $VK_DRIVER_FILES"
        runner_record 'browser_platform wayland'
        ;;
esac
if [ "$mode" = pages ]; then
    # shellcheck disable=SC2034 # common.sh loads the private module
    RUNNER_SEAT_MODULE=$here/seat.so
fi
if [ "$mode" = gpu ]; then
    # shellcheck disable=SC2034 # common.sh starts Weston with it
    RUNNER_RENDERER=gl
fi
if [ "$mode" = webgpu-hasvk ] || [ "$mode" = webgpu-frames ] || [ "$mode" = webgpu-trace ]; then
    [ "$browser" = chromium ] || runner_die "hasvk needs Chromium"
    [ -S "${XDG_RUNTIME_DIR:?}/${WAYLAND_DISPLAY:?}" ] || runner_die "no caller's Wayland display"
    runner_record "compositor caller's DRM Weston on the Lenovo, see ../weston.log"
    runner_record "icd ${VK_DRIVER_FILES:?}"
else
    runner_weston
fi
python3 "$here/run.py" "$browser" "$mode" "$out" "$RUNNER_WORK"
runner_finish
