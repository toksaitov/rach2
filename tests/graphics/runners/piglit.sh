#!/bin/sh
# piglit.sh: parts of piglit's quick profile, or the tests of a list, on the
# guest's Mesa, inside a graphics guest, Rach2's or Debian's, as an ordinary
# user.
#
#   piglit.sh [-g|-z ICD] [-p surfaceless|gbm] [-o OUT] PART...|all
#   piglit.sh [-g|-z ICD] [-p surfaceless|gbm] [-o OUT] -l LIST [-e N] [-s START]
#
# Runs the tests of piglit's `quick` profile whose names each PART's pattern
# matches, or every QEMU part's with `all`, or with -l the tests LIST names,
# by `piglit run`. On the surfaceless EGL platform, the default, it draws
# into buffers of its own with no display server, the same in both guests
# (Rach2's root has no Xvfb); GL tests that need a window system skip or
# crash there, on both sides alike. `-p gbm` runs on the machine's own GPU
# through GBM, as step 9 of the second discovery run took Linux's reference
# on the Lenovo, with EGL_PLATFORM=surfaceless, as its reruns of the EGL
# tests had it, so that a test that opens EGL's default display does not
# take X11's. PIGLIT_NO_FAST_SKIP=1 makes each test check its own
# requirements: piglit's fast skip asks wflinfo for the GL versions, which
# it does not give on the surfaceless platform and which Rach2's root lacks,
# and skips every test that names one.
#
# On the surfaceless platform the tests draw on llvmpipe, which
# LIBGL_ALWAYS_SOFTWARE=1 names, so that EGL tries no render node of the
# display first, a test process on each CPU (`piglit run -c`); with -g they
# draw on the device EGL finds, the guest's GPU, virgl under the virgl
# display profile, one test at a time (`piglit run -1`): QEMU draws every
# context of the guest in one thread, so tests at once gain little there,
# and on the testing PC eight heavy ones at once lose the host's D3D12
# device, which ends QEMU (dev-log/77-vgpu/upstream.md). With -z, on the
# surfaceless platform alone, they draw on zink over the Vulkan driver that
# the ICD file ICD names, alone, a test process on each CPU:
# LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=zink VK_DRIVER_FILES=ICD. Zink
# takes a device of the CPU type only in software mode, and Venus over the
# host's lavapipe is one; without LIBGL_ALWAYS_SOFTWARE zink fails with
# "failed to choose pdev". Through GBM they run a process on each CPU, or
# one at a time with -g. The record's
# `renderer` line is what `eglinfo -B` names on the same platform with the
# same environment: the renderer, and the OpenGL and OpenGL ES versions.
#
# The parts, by the name of the patterns:
#
#   gl-1.0 gl-1.1  the OpenGL 1.0 and 1.1 tests
#   glsl-1.10 glsl-1.20  the GLSL 1.10 and 1.20 tests, compiles and shader
#                runs
#   fbo          ARB_framebuffer_object
#   sync         GL and EGL fences: ARB_sync, EGL_KHR_fence_sync and
#                EGL_ANDROID_native_fence_sync, which takes sync files
#   dmabuf       EXT_image_dma_buf_import, which takes dma-bufs
#
# and the Lenovo's five, which together are the whole profile, each some 4
# to 10 minutes of Linux's run there:
#
#   quick-glsl1  the GLSL 1.x tests
#   quick-glsl2  the other GLSL versions and the GLSL and assembly parsers
#   quick-arb1   the ARB extensions from a to g
#   quick-arb2   the ARB extensions from h to z
#   quick-rest   everything else
#
# The `all` of the QEMU suites runs the first seven.
#
# LIST has one test's name a line, as `piglit print-cmd --format '{name}'`
# gives it; a line that begins with `#` is a comment, and the file is
# compressed with gzip when its name ends in .gz. Of its tests the runner
# takes every Nth (1, every test), counted from the STARTth (1, the first),
# so that START 1 to N of the same N run the list once in N parts. The
# sample goes to piglit as its --test-list, but for the names that hold a
# `#`, where piglit cuts a line of that file: those run in a second `piglit
# run`, each named by a pattern of its own, and the two runs' results are
# one. A name of LIST that the profile lacks ends piglit, and the run fails.
#
# Run it with guest-env.sh's conformance class, LP_NUM_THREADS=0. piglit
# ends one of its own GL tests after 60 s, as `timeout`, a result like any
# other, and piglit.sh sets no limit of its own.
#
# OUT (.) gets the record, and for each part, or for the list under the
# name of its file without `.txt` and `.gz`, NAME.json, piglit's results
# whole (NAME-2.json the second run's), and NAME.results, one line a test
# or subtest in the order of their names: `NAME RESULT`, NAME being
# piglit's, which may hold blanks, with `@SUBTEST` for a subtest, and RESULT
# piglit's: pass, skip, fail, warn, crash, timeout, dmesg-warn, dmesg-fail,
# incomplete or notrun. A list also leaves NAME.sample, the names it ran,
# and a part PART.scope, its patterns in judge.py's SCOPE form. Each part or
# list prints one line:
#
#   piglit: result: PART RESULTS results: pass P skip S fail F other O
#   piglit: result: NAME tests T of M every N from START, RESULTS results:
#       pass P skip S fail F other O
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
        quick-glsl1) echo '^spec@glsl-1\.' ;;
        quick-glsl2) echo '^(spec@glsl-[34e]|glslparsertest@|asmparsertest@)' ;;
        quick-arb1) echo '^spec@arb_[a-g]' ;;
        quick-arb2) echo '^spec@arb_[h-z]' ;;
        quick-rest) echo '.' ;;
        *) return 1 ;;
    esac
}

# others PART: the parts whose tests PART's pattern takes too and leaves to
# them.
others()
{
    [ "$1" != quick-rest ] || echo quick-glsl1 quick-glsl2 quick-arb1 quick-arb2
}
parts='gl-1.0 gl-1.1 glsl-1.10 glsl-1.20 fbo sync dmabuf'

usage()
{
    echo "usage: piglit.sh [-g|-z ICD] [-p surfaceless|gbm] [-o OUT] PART...|all" \
        "| piglit.sh [-g|-z ICD] [-p surfaceless|gbm] [-o OUT] -l LIST [-e N] [-s START]" >&2
    exit 2
}

out=. gpu=0 icd='' list='' every=1 start=1 platform=surfaceless
while getopts o:gz:p:l:e:s: o; do
    case $o in
        o) out=$OPTARG ;;
        p) platform=$OPTARG ;;
        g) gpu=1 ;;
        z) icd=$OPTARG ;;
        l) list=$OPTARG ;;
        e) every=$OPTARG ;;
        s) start=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
case $every$start in *[!0-9]*) usage ;; esac
case $platform in surfaceless | gbm) ;; *) usage ;; esac
[ -z "$icd" ] || { [ "$gpu" -eq 0 ] && [ "$platform" = surfaceless ]; } || usage
[ -z "$icd" ] || [ -f "$icd" ] || { echo "piglit: no ICD file $icd" >&2; exit 1; }
if [ -n "$list" ]; then
    [ $# -eq 0 ] && [ "$every" -ge 1 ] && [ "$start" -ge 1 ] && [ "$start" -le "$every" ] || usage
    [ -f "$list" ] || { echo "piglit: no list $list" >&2; exit 1; }
else
    [ $# -ge 1 ] && [ "$every$start" = 11 ] || usage
    # shellcheck disable=SC2086 # the parts are words
    [ "$*" != all ] || set -- $parts
    for part; do
        pattern "$part" > /dev/null || { echo "piglit: no part $part; $parts" >&2; exit 2; }
    done
fi
command -v piglit > /dev/null || { echo "piglit: no piglit" >&2; exit 1; }

# piglit_version: the version of Debian's piglit, from dpkg where the guest
# has its database.
piglit_version()
{
    dpkg-query -W -f '${Version}' piglit 2> /dev/null || echo unknown
}

runner_init piglit "$out"
runner_start
if [ -n "$list" ]; then
    runner_record "case piglit quick every $every from $start of $list"
else
    runner_record "case piglit quick $*"
fi
runner_record "warmup 0"
runner_record "repetitions 1"
PIGLIT_COMPRESSION=none PIGLIT_NO_FAST_SKIP=1
export PIGLIT_COMPRESSION PIGLIT_NO_FAST_SKIP
if [ "$gpu" -eq 1 ]; then
    at_once=-1 at_once_text='one test at a time'
else
    at_once=-c at_once_text='a process on each CPU'
fi
if [ "$platform" = gbm ]; then
    piglit_platform=gbm
    EGL_PLATFORM=surfaceless
    export EGL_PLATFORM
    runner_record "env PIGLIT_NO_FAST_SKIP=1 EGL_PLATFORM=surfaceless"
elif [ "$gpu" -eq 1 ]; then
    piglit_platform=surfaceless_egl
    runner_record "env PIGLIT_NO_FAST_SKIP=1"
elif [ -n "$icd" ]; then
    piglit_platform=surfaceless_egl
    LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=zink VK_DRIVER_FILES=$icd
    export LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER VK_DRIVER_FILES
    runner_record "env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=zink VK_DRIVER_FILES=$icd PIGLIT_NO_FAST_SKIP=1"
else
    piglit_platform=surfaceless_egl
    LIBGL_ALWAYS_SOFTWARE=1
    export LIBGL_ALWAYS_SOFTWARE
    runner_record "env LIBGL_ALWAYS_SOFTWARE=1 PIGLIT_NO_FAST_SKIP=1"
fi
runner_record "piglit $(piglit_version), platform $piglit_platform, $at_once_text"
runner_record "renderer $(eglinfo -B -p "$platform" 2> /dev/null | awk -F': ' '
    /^OpenGL core profile renderer:/ { r = $2 }
    /^OpenGL core profile version:/ { v = $2 }
    /^OpenGL ES profile version:/ { e = $2 }
    END { print (r == "" ? "unknown" : r "; OpenGL " v "; " e) }')"

# run_piglit LOG RESULTS ARG...: one `piglit run` of the quick profile with
# the ARGs, its results directory RESULTS and its output LOG.
run_piglit()
{
    log=$1 dir=$2
    shift 2
    piglit run "$at_once" -o -p "$piglit_platform" -l dummy "$@" quick "$dir" > "$log" 2>&1
}

# summarize NAME WHAT JSON...: write NAME.results from piglit's results in
# the JSONs and print the result line, WHAT after NAME.
summarize()
{
    python3 -I - "$out/$1.results" "$@" << 'EOF' | tee -a "$out/results"
import json, sys
rows = []
for path in sys.argv[4:]:
    tests = json.load(open(path, encoding="utf-8"))["tests"]
    for name, t in tests.items():
        rows.append((name, t["result"]))
        for sub, result in t.get("subtests", {}).items():
            if sub != "__type__":
                rows.append(("%s@%s" % (name, sub), result))
counts = {"pass": 0, "skip": 0, "fail": 0}
other = 0
with open(sys.argv[1], "w", encoding="utf-8") as f:
    for name, result in sorted(rows):
        f.write("%s %s\n" % (name, result))
        if result in counts:
            counts[result] += 1
        else:
            other += 1
print("piglit: result: %s%s %d results: pass %d skip %d fail %d other %d" %
      (sys.argv[2], sys.argv[3], len(rows), counts["pass"], counts["skip"], counts["fail"], other))
EOF
}

status=0
if [ -n "$list" ]; then
    name=${list##*/}
    name=${name%.gz}
    name=${name%.txt}
    case $list in
        *.gz) gzip -d < "$list" ;;
        *) cat "$list" ;;
    esac | sed -e '/^#/d' -e '/^$/d' > "$RUNNER_WORK/list"
    all=$(wc -l < "$RUNNER_WORK/list" | tr -d ' ')
    awk -v e="$every" -v s="$start" '(NR - s) % e == 0 && NR >= s' "$RUNNER_WORK/list" > "$out/$name.sample"
    tests=$(wc -l < "$out/$name.sample" | tr -d ' ')
    [ "$tests" -gt 0 ] || runner_die "the list $list has no test"
    runner_record "list $list $all tests sha256 $(sha256sum < "$RUNNER_WORK/list" | awk '{ print $1 }')"
    runner_record "sample $tests tests sha256 $(sha256sum < "$out/$name.sample" | awk '{ print $1 }')"
    grep -v '#' "$out/$name.sample" > "$RUNNER_WORK/plain" || true
    grep '#' "$out/$name.sample" > "$RUNNER_WORK/marked" || true
    t0=$(date +%s)
    jsons=
    if [ -s "$RUNNER_WORK/plain" ]; then
        if run_piglit "$out/$name.log" "$RUNNER_WORK/$name" --test-list "$RUNNER_WORK/plain"; then
            cp "$RUNNER_WORK/$name/results.json" "$out/$name.json"
            jsons=$out/$name.json
        else
            echo "piglit: $name: piglit run failed: $(tail -n 3 "$out/$name.log")" >&2
            status=1
        fi
    fi
    if [ -s "$RUNNER_WORK/marked" ] && [ "$status" -eq 0 ]; then
        # piglit cuts a line of its --test-list at `#`, so each of these
        # names is a pattern of its own.
        python3 -I - "$RUNNER_WORK/marked" > "$RUNNER_WORK/marked.args" << 'EOF'
import re, sys
for line in open(sys.argv[1], encoding="utf-8"):
    print("-t\n^%s$" % re.escape(line.rstrip("\n")))
EOF
        set --
        while IFS= read -r arg; do
            set -- "$@" "$arg"
        done < "$RUNNER_WORK/marked.args"
        if run_piglit "$out/$name-2.log" "$RUNNER_WORK/$name-2" "$@"; then
            cp "$RUNNER_WORK/$name-2/results.json" "$out/$name-2.json"
            jsons="$jsons $out/$name-2.json"
        else
            echo "piglit: $name: piglit run failed: $(tail -n 3 "$out/$name-2.log")" >&2
            status=1
        fi
    fi
    t1=$(date +%s)
    runner_record "wall $((t1 - t0)) s"
    if [ "$status" -eq 0 ]; then
        # shellcheck disable=SC2086 # the results' files are words
        summarize "$name" " tests $tests of $all every $every from $start," $jsons
    fi
else
    for part; do
        set -- -t "$(pattern "$part")"
        echo "~$(pattern "$part")" > "$out/$part.scope"
        for other in $(others "$part"); do
            set -- "$@" -x "$(pattern "$other")"
            echo "!$(pattern "$other")" >> "$out/$part.scope"
        done
        runner_record "part $part $*"
        if ! run_piglit "$out/$part.log" "$RUNNER_WORK/$part" "$@"; then
            echo "piglit: $part: piglit run failed: $(tail -n 3 "$out/$part.log")" >&2
            status=1
            continue
        fi
        cp "$RUNNER_WORK/$part/results.json" "$out/$part.json"
        summarize "$part" "" "$out/$part.json"
    done
fi
runner_finish
exit "$status"
