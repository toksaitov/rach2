#!/bin/sh
# piglit.sh: parts of piglit's quick profile on the guest's Mesa, inside a
# graphics guest, Rach2's or Debian's, as an ordinary user.
#
#   piglit.sh [-o OUT] PART...|all
#
# Runs the tests of piglit's `quick` profile whose names each PART's pattern
# matches, or every part's with `all`, by `piglit run -c`, a test process on
# each CPU, on the surfaceless EGL platform, which draws into buffers of its
# own with no display server, the same in both guests (Rach2's root has no
# Xvfb), on llvmpipe, which LIBGL_ALWAYS_SOFTWARE=1 names, so that EGL tries
# no render node of the display first. GL tests that need a window system
# skip or crash there, on both sides alike. The
# parts, by the name of the patterns:
#
#   gl-1.0 gl-1.1  the OpenGL 1.0 and 1.1 tests
#   glsl-1.10 glsl-1.20  the GLSL 1.10 and 1.20 tests, compiles and shader
#                runs
#   fbo          ARB_framebuffer_object
#   sync         GL and EGL fences: ARB_sync, EGL_KHR_fence_sync and
#                EGL_ANDROID_native_fence_sync, which takes sync files
#   dmabuf       EXT_image_dma_buf_import, which takes dma-bufs
#
# Run it with guest-env.sh's conformance class, LP_NUM_THREADS=0. piglit
# sets no time limit on a test, and piglit.sh none of its own.
#
# OUT (.) gets the record, `results.json`, piglit's results whole, and
# PART.results for each part, one line a test or subtest in the order of
# their names: `NAME RESULT`, NAME being piglit's, which may hold blanks, with
# `@SUBTEST` for a subtest, and RESULT piglit's: pass, skip, fail, warn,
# crash, timeout, dmesg-warn, dmesg-fail, incomplete or notrun. Each part
# prints one line:
#
#   piglit: result: PART RESULTS results: pass P skip S fail F other O
#
# The status is 0 when piglit made each part's results and 1 otherwise; a
# test's failure is a result, which judge.py compares with the reference's.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

# pattern PART: the regular expression of PART, which piglit's -t matches
# against the tests' names.
pattern()
{
    case $1 in
        gl-1.0) echo '^spec@!opengl 1\.0@' ;;
        gl-1.1) echo '^spec@!opengl 1\.1@' ;;
        glsl-1.10) echo '^spec@glsl-1\.10@' ;;
        glsl-1.20) echo '^spec@glsl-1\.20@' ;;
        fbo) echo '^spec@arb_framebuffer_object@' ;;
        sync) echo '^spec@(arb_sync|egl_khr_fence_sync|egl_android_native_fence_sync)@' ;;
        dmabuf) echo '^spec@ext_image_dma_buf_import@' ;;
        *) return 1 ;;
    esac
}
parts='gl-1.0 gl-1.1 glsl-1.10 glsl-1.20 fbo sync dmabuf'

out=.
while getopts o: o; do
    case $o in
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { echo "usage: piglit.sh [-o OUT] PART...|all" >&2; exit 2; }
# shellcheck disable=SC2086 # the parts are words
[ "$*" != all ] || set -- $parts
for part; do
    pattern "$part" > /dev/null || { echo "piglit: no part $part; $parts" >&2; exit 2; }
done
command -v piglit > /dev/null || { echo "piglit: no piglit" >&2; exit 1; }

runner_init piglit "$out"
runner_start
runner_record "case piglit quick $*"
runner_record "warmup 0"
runner_record "repetitions 1"
runner_record "piglit $(dpkg-query -W -f '${Version}' piglit 2> /dev/null || echo unknown), platform surfaceless_egl, a process on each CPU"
PIGLIT_COMPRESSION=none LIBGL_ALWAYS_SOFTWARE=1
export PIGLIT_COMPRESSION LIBGL_ALWAYS_SOFTWARE
runner_record "env LIBGL_ALWAYS_SOFTWARE=1"

status=0
for part; do
    runner_record "part $part $(pattern "$part")"
    if ! piglit run -c -o -p surfaceless_egl -l dummy -t "$(pattern "$part")" quick \
        "$RUNNER_WORK/$part" > "$out/$part.log" 2>&1; then
        echo "piglit: $part: piglit run failed: $(tail -n 3 "$out/$part.log")" >&2
        status=1
        continue
    fi
    cp "$RUNNER_WORK/$part/results.json" "$out/$part.json"
    python3 -I - "$out/$part.json" "$part" "$out/$part.results" << 'EOF' | tee -a "$out/results"
import json, sys
tests = json.load(open(sys.argv[1], encoding="utf-8"))["tests"]
rows = []
for name, t in tests.items():
    rows.append((name, t["result"]))
    for sub, result in t.get("subtests", {}).items():
        if sub != "__type__":
            rows.append(("%s@%s" % (name, sub), result))
counts = {"pass": 0, "skip": 0, "fail": 0}
other = 0
with open(sys.argv[3], "w", encoding="utf-8") as f:
    for name, result in sorted(rows):
        f.write("%s %s\n" % (name, result))
        if result in counts:
            counts[result] += 1
        else:
            other += 1
print("piglit: result: %s %d results: pass %d skip %d fail %d other %d" %
      (sys.argv[2], len(rows), counts["pass"], counts["skip"], counts["fail"], other))
EOF
done
runner_finish
exit "$status"
