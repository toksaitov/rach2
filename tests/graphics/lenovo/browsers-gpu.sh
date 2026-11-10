#!/bin/sh
# browsers-gpu.sh: the checks of the accelerated browsers' step, run as root
# on Rach2 or Debian on the HD 4400, or on Rach2's fake i915 chip in QEMU.
#
#   sh browsers-gpu.sh [-f] [-d D3D] USER OUT
#
# USER is the ordinary account that runs the programs, a member of render
# and video; OUT gets a directory of evidence for each check. The graphics
# payload is the directory above this script's, /tmp/graphics in the cases,
# and D3D, /tmp/d3d by default, holds tests/graphics/d3d's bundle, its
# probes and run.py. The checks:
#
#   chromium  browser/run.py's GPU profile under a headless Weston on GL,
#             read by browser/sandbox.py: WebGL's unmasked renderer and its
#             pixel, chrome://gpu's GL_RENDERER and WebGL status, the driver
#             Mesa's loader names in the GPU process, and each process's
#             sandbox, judged by browser/judge.py against Debian's sandbox
#             case on the same binaries.
#   firefox   the same profile: WebGL's renderer and pixel, about:support's
#             compositor, GPU and driver, and its sandbox table.
#   x11       Xwayland under the same Weston: glxinfo -B names the renderer
#             with direct rendering, and glmark2 --validate draws its build
#             and texture scenes and reads each back.
#   d3d9      d3d9probe through Proton's Wine on WineD3D's GL renderer on
#             that Xwayland: the adapter it names and the two pixels it
#             reads back.
#
# Each check prints `browsers-gpu: result: CHECK pass` or `... fail:
# DETAIL`, and the script `browsers-gpu: result: pass` and exits 0 when all
# four passed. -f is the fake chip, which completes every batch without
# drawing: the readbacks are printed and not judged, and a check fails only
# when its program stops short of its end or names another renderer.
set -eu

# The renderer Crocus names on the HD 4400, and the identity WineD3D reads.
CROCUS='HD Graphics 4400'
D3D_ID='vendor 0x8086 device 0x0a16'

here=$(cd "$(dirname "$0")" && pwd)
graphics=$(cd "$here/.." && pwd)

# x11 OUT: as the ordinary user, start Weston with Xwayland on GL and run the
# X11 GL program and the Direct3D 9 probe on its display, each writing its
# evidence under OUT; the caller judges it.
x11()
{
    RUNNER_DIR=$graphics/runners
    . "$RUNNER_DIR/common.sh"
    runner_init browsers-gpu "$1/x11"
    runner_start plain
    # shellcheck disable=SC2034 # common.sh starts Weston with them
    RUNNER_RENDERER=gl RUNNER_XWAYLAND=1
    runner_weston
    set +e
    glxinfo -B > "$1/x11/glxinfo.txt" 2>&1
    echo "exit $?" >> "$1/x11/glxinfo.txt"
    glmark2 --validate -b build -b texture > "$1/x11/glmark2.txt" 2>&1
    echo "exit $?" >> "$1/x11/glmark2.txt"
    python3 "$2/run.py" -a host probe wined3d/9 "$1/d3d9" > "$1/d3d9.log" 2>&1
    echo "exit $?" >> "$1/d3d9.log"
    set -e
    runner_finish
}

if [ "${1:-}" = x11 ]; then
    shift
    x11 "$@"
    exit 0
fi

fake='' d3d=/tmp/d3d
while getopts fd: opt; do
    case $opt in
        f) fake=fake ;;
        d) d3d=$OPTARG ;;
        *) echo 'usage: browsers-gpu.sh [-f] [-d D3D] USER OUT' >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# = 2 ] || { echo 'usage: browsers-gpu.sh [-f] [-d D3D] USER OUT' >&2; exit 2; }
user=$1 out=$2
[ "$(id -u)" = 0 ] || { echo 'browsers-gpu: run as root, which reads the sandboxes' >&2; exit 2; }
mkdir -p "$out" /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix
chown "$user" "$out"
failed=0

# verdict CHECK DETAIL...: print the check's line; an empty DETAIL passes.
verdict()
{
    check=$1
    shift
    if [ -z "$*" ]; then
        echo "browsers-gpu: result: $check pass"
    else
        echo "browsers-gpu: result: $check fail: $*"
        failed=1
    fi
}

# Each browser's run, then its judge, whose last line is printed.
for browser in chromium firefox; do
    why=''
    if [ "$browser" = chromium ]; then
        python3 "$graphics/browser/sandbox.py" "$user" chromium "$out/chromium" gpu \
            > "$out/chromium.log" 2>&1 || why="the run failed: $(tail -n 1 "$out/chromium.log")"
    else
        su -s /bin/sh "$user" -c "GRAPHICS_CLASS=conformance GRAPHICS_DONE=1; \
            export GRAPHICS_CLASS GRAPHICS_DONE; . '$graphics/guest-env.sh' && \
            sh '$graphics/browser/run.sh' firefox gpu '$out/firefox'" \
            > "$out/firefox.log" 2>&1 || why="the run failed: $(tail -n 1 "$out/firefox.log")"
    fi
    if [ -z "$why" ]; then
        python3 "$graphics/browser/judge.py" "$browser" gpu "$graphics/browser/ref" \
            "$out/$browser" $fake > "$out/$browser.judge" 2>&1 ||
            why="the judge failed: $(tail -n 1 "$out/$browser.judge")"
    fi
    [ ! -f "$out/$browser/result.json" ] || {
        echo "browsers-gpu: evidence: $browser"
        cat "$out/$browser/result.json"
    }
    verdict "$browser" "$why"
done

su -s /bin/sh "$user" -c "sh '$here/browsers-gpu.sh' x11 '$out' '$d3d'" > "$out/x11.log" 2>&1 ||
    true
for f in x11/glxinfo.txt x11/glmark2.txt d3d9.log d3d9/pixels.txt; do
    [ ! -f "$out/$f" ] || { echo "browsers-gpu: evidence: $f"; cat "$out/$f"; }
done

why=''
glx=$out/x11/glxinfo.txt glmark=$out/x11/glmark2.txt
if [ ! -f "$glx" ] || [ ! -f "$glmark" ]; then
    why="Weston with Xwayland did not start: $(tail -n 1 "$out/x11.log")"
else
    grep -q '^exit 0$' "$glx" || why="glxinfo failed"
    grep -q 'direct rendering: Yes' "$glx" || why="$why${why:+; }no direct rendering"
    grep 'OpenGL renderer string:' "$glx" | grep -q "$CROCUS" ||
        why="$why${why:+; }the renderer is not Crocus's"
    grep -q '^exit 0$' "$glmark" || why="$why${why:+; }glmark2 failed"
    good=$(grep -c 'Validation: Success' "$glmark" || true)
    seen=$(grep -c 'Validation: ' "$glmark" || true)
    [ "$seen" = 2 ] || why="$why${why:+; }glmark2 validated $seen scenes, not 2"
    [ -n "$fake" ] || [ "$good" = 2 ] || why="$why${why:+; }glmark2 read back $good of 2 scenes right"
fi
verdict x11 "$why"

why=''
pixels=$out/d3d9/pixels.txt
if [ ! -f "$pixels" ]; then
    why="the probe wrote no pixels: $(tail -n 2 "$out/d3d9.log" | head -n 1)"
else
    grep '^adapter: ' "$pixels" | grep -q "$D3D_ID" || why="WineD3D's adapter is not the HD 4400"
    if [ -z "$fake" ]; then
        grep -q '^D3D9 OK$' "$pixels" || why="$why${why:+; }the pixels are wrong"
    elif ! grep -q -E '^center .* draw (OK|WRONG)$' "$pixels"; then
        why="$why${why:+; }the probe stopped before its readback"
    fi
fi
verdict d3d9 "$why"

if [ "$failed" = 0 ]; then
    echo "browsers-gpu: result: pass${fake:+ on the fake chip, readbacks not judged}"
    exit 0
fi
echo "browsers-gpu: result: fail"
exit 1
