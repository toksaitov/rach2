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
runner_start
runner_record "case $browser $mode"
if [ "$mode" = pages ]; then
    # shellcheck disable=SC2034 # common.sh loads the private module
    RUNNER_SEAT_MODULE=$here/seat.so
fi
runner_weston
python3 "$here/run.py" "$browser" "$mode" "$out" "$RUNNER_WORK"
runner_finish
