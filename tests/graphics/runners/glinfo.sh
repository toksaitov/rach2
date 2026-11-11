#!/bin/sh
# glinfo.sh: the OpenGL renderer a guest with a GPU gives its programs, as
# eglinfo and glxinfo name it, inside a graphics guest, Rach2's or Debian's,
# as an ordinary user.
#
#   glinfo.sh [-o OUT]
#
# Runs `eglinfo -B -p surfaceless`, EGL on the device alone, as piglit.sh -g
# and deqp.sh draw; then starts a headless Weston with its GL renderer, an
# Xwayland on it, as a desktop session's X programs have, and `glxinfo -B`
# there. Xwayland's glamor draws on the GPU when the compositor offers
# linux-dmabuf, which Weston's GL renderer does and Pixman does not: under
# Pixman glxinfo names llvmpipe on a guest whose EGL names virgl.
#
# OUT (.) gets the record; eglinfo.out and glxinfo.out, the programs' output
# whole; and `renderers`, the lines of both that name a renderer, a version
# of OpenGL or OpenGL ES, or direct and accelerated rendering, each after
# `eglinfo: ` or `glxinfo: ` and without Mesa's own version, which differs
# between guests. The case compares `renderers` with the Debian guest's. It
# prints one line:
#
#   glinfo: result: egl RENDERER, glx RENDERER
#
# The status is 0 when both programs named a renderer.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

out=.
while getopts o: o; do
    case $o in
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || { echo "usage: glinfo.sh [-o OUT]" >&2; exit 2; }
for program in eglinfo glxinfo weston Xwayland; do
    command -v "$program" > /dev/null || { echo "glinfo: no $program" >&2; exit 1; }
done

# cleanup: end Xwayland, then what common.sh's cleanup ends; installed as
# the exit trap.
cleanup()
{
    if [ -n "$xwayland" ]; then
        kill -TERM "$xwayland" 2>/dev/null || true
        wait "$xwayland" 2>/dev/null || true
    fi
    runner_cleanup
}

# named PROGRAM FILE: the lines of FILE, PROGRAM's output, that `renderers`
# keeps.
named()
{
    sed -n -E -e 's/[[:space:]]+$//' -e 's/ Mesa [^ ]*$//' -e 's/^[[:space:]]+//' \
        -e '/^(OpenGL .*(renderer|version)( string)?:|direct rendering:|Accelerated:|Max .* version:)/p' \
        "$2" | sed "s/^/$1: /"
}

xwayland=''
runner_init glinfo "$out"
trap cleanup EXIT
runner_start plain
runner_record "case glinfo eglinfo on surfaceless EGL, glxinfo on Xwayland under a headless Weston with its GL renderer"
eglinfo -B -p surfaceless > "$out/eglinfo.out" 2>&1 ||
    runner_die "eglinfo failed: $(tail -n 3 "$out/eglinfo.out")"

# shellcheck disable=SC2034 # runner_weston reads it
RUNNER_RENDERER=gl
runner_weston
runner_record "weston $(sed -n 's/.*GL renderer: *//p' "$out/weston.log" | head -n 1)"
Xwayland -displayfd 3 -noreset 3> "$RUNNER_WORK/display" > "$out/xwayland.log" 2>&1 < /dev/null &
xwayland=$!
until [ -s "$RUNNER_WORK/display" ]; do
    kill -0 "$xwayland" 2>/dev/null || runner_die "Xwayland ended: $(tail -n 3 "$out/xwayland.log")"
    sleep 0.1
done
DISPLAY=:$(cat "$RUNNER_WORK/display") glxinfo -B > "$out/glxinfo.out" 2>&1 ||
    runner_die "glxinfo failed: $(tail -n 3 "$out/glxinfo.out")"

{
    named eglinfo "$out/eglinfo.out"
    named glxinfo "$out/glxinfo.out"
} > "$out/renderers"
egl=$(sed -n 's/^eglinfo: OpenGL core profile renderer: //p' "$out/renderers" | head -n 1)
glx=$(sed -n 's/^glxinfo: OpenGL renderer string: //p' "$out/renderers" | head -n 1)
runner_record "renderer $egl"
runner_record "renderer glx $glx"
runner_finish
[ -n "$egl" ] && [ -n "$glx" ] || runner_die "eglinfo or glxinfo named no renderer"
echo "glinfo: result: egl $egl, glx $glx" | tee -a "$out/results"
