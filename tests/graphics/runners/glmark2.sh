#!/bin/sh
# glmark2.sh: glmark2's scenes validated and scored on the guest's Mesa,
# inside a graphics guest, Rach2's or Debian's, as an ordinary user.
#
#   glmark2.sh [-a gl|es2] [-m both|validate|benchmark] [-o OUT]
#
# Runs glmark2-wayland, the desktop GL build, or glmark2-es2-wayland with
# `-a es2`, twice under a headless Weston of its own, off screen at the
# fixed 1280x800: first with --validate, which draws each scene of the
# default benchmark list once and compares the frame with glmark2's own
# reference, giving Success, Failure or Unknown where it has none, and then
# as the benchmark, which gives each scene's frames per second and the
# score. `-m validate` or `-m benchmark` runs the one alone. With
# RUNNER_SESSION=1 (common.sh) it runs fullscreen on the session's
# compositor instead, as `glmark2-wayland --fullscreen` took Linux's scores
# on the Lenovo. Run it with guest-env.sh's performance class, LP_NUM_THREADS
# at the guest's CPU count. glmark2 runs each scene for its own fixed time;
# glmark2.sh sets no time limit of its own.
#
# OUT (.) gets the record, `validations`, one line a scene in glmark2's
# order: `SCENE RESULT`, SCENE being glmark2's name and options with the
# blanks taken out, as `[build]use-vbo=false`, and `scores`, the same with
# each scene's frames per second and a last line `score N`. It prints one
# line for each run:
#
#   glmark2: result: SCENES scenes: Success S Failure F Unknown U
#   glmark2: result: score N
#
# The status is 0 when each run ended with its list whole and 1 otherwise;
# judge.py compares the validations with the reference's, and the score is
# recorded alone, since in QEMU it measures the host's load as much as the
# guest; on the Lenovo the speed gate (judge.py gate) compares it.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

api=gl mode=both out=.
while getopts a:m:o: o; do
    case $o in
        a) api=$OPTARG ;;
        m) mode=$OPTARG ;;
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
case $api:$mode in
    gl:* | es2:*) ;;
    *) set -- usage ;;
esac
case $mode in both | validate | benchmark) ;; *) set -- usage ;; esac
[ $# -eq 0 ] || { echo "usage: glmark2.sh [-a gl|es2] [-m both|validate|benchmark] [-o OUT]" >&2; exit 2; }
program=glmark2-wayland
[ "$api" = gl ] || program=glmark2-es2-wayland
command -v $program > /dev/null || { echo "glmark2: no $program" >&2; exit 1; }

runner_init glmark2 "$out"
runner_start
runner_record "case glmark2 the default benchmark list, $mode, $program"
runner_record "warmup 0"
runner_record "repetitions 1 of each run"
runner_record "glmark2 $($program --version 2>&1 | head -n 1)"
runner_weston
if [ "${RUNNER_SESSION:-0}" = 1 ]; then
    set -- --fullscreen
else
    set -- --off-screen -s "${RUNNER_WIDTH}x$RUNNER_HEIGHT"
fi

if [ "$mode" != benchmark ]; then
    $program "$@" --validate > "$out/validate.log" 2>&1 ||
        runner_die "$program --validate failed: $(tail -n 3 "$out/validate.log")"
    sed -n 's/^ *\(\[[^]]*\]\) \(.*\): Validation: \([A-Za-z]*\).*/\1\2 \3/p' "$out/validate.log" |
        tr -d ' ' | sed 's/\(.*\)\(Success\|Failure\|Unknown\)$/\1 \2/' > "$out/validations"
    runner_record "renderer $(sed -n 's/^ *GL_RENDERER: *//p' "$out/validate.log" | head -n 1)"
    [ -s "$out/validations" ] || runner_die "$program gave no validations"
fi
if [ "$mode" != validate ]; then
    $program "$@" > "$out/benchmark.log" 2>&1 ||
        runner_die "$program failed: $(tail -n 3 "$out/benchmark.log")"
    sed -n 's/^ *\(\[[^]]*\]\) \(.*\): FPS: \([0-9]*\).*/\1\2 \3/p' "$out/benchmark.log" |
        sed 's/ \([0-9]*\)$/@\1/' | tr -d ' ' | tr '@' ' ' > "$out/scores"
    score=$(sed -n 's/.*glmark2 Score: *\([0-9]*\).*/\1/p' "$out/benchmark.log" | tail -n 1)
    echo "score ${score:-none}" >> "$out/scores"
    runner_record "renderer $(sed -n 's/^ *GL_RENDERER: *//p' "$out/benchmark.log" | head -n 1)"
    runner_record "surface $(sed -n 's/^ *Surface Size: *//p' "$out/benchmark.log" | head -n 1)"
    [ -n "$score" ] || runner_die "$program gave no score"
fi
runner_finish
count()
{
    grep -c " $1\$" "$out/validations" || true
}
if [ "$mode" != benchmark ]; then
    echo "glmark2: result: $(wc -l < "$out/validations") scenes: Success $(count Success)" \
        "Failure $(count Failure) Unknown $(count Unknown)" | tee -a "$out/results"
fi
if [ "$mode" != validate ]; then
    echo "glmark2: result: score $score" | tee -a "$out/results"
fi
