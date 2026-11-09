#!/bin/sh
# glmark2.sh: glmark2's scenes validated and scored on the guest's Mesa,
# inside a graphics guest, Rach2's or Debian's, as an ordinary user.
#
#   glmark2.sh [-o OUT]
#
# Runs glmark2-wayland, the desktop GL build, twice under a headless Weston
# of its own, off screen at the fixed 1280x800: first with --validate, which
# draws each scene of the default benchmark list once and compares the frame
# with glmark2's own reference, giving Success, Failure or Unknown where it
# has none, and then as the benchmark, which gives each scene's frames per
# second and the score. Run it with guest-env.sh's performance class,
# LP_NUM_THREADS at the guest's CPU count. glmark2 runs each scene for its
# own fixed time; glmark2.sh sets no time limit of its own.
#
# OUT (.) gets the record, `validations`, one line a scene in glmark2's
# order: `SCENE RESULT`, SCENE being glmark2's name and options with the
# blanks taken out, as `[build]use-vbo=false`, and `scores`, the same with
# each scene's frames per second and a last line `score N`. It prints one
# line:
#
#   glmark2: result: SCENES scenes: Success S Failure F Unknown U score N
#
# The status is 0 when both runs ended with their lists whole and 1
# otherwise; judge.py compares the validations with the reference's, and
# the score is recorded alone, since it measures the host's load as much as
# the guest.
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
[ $# -eq 0 ] || { echo "usage: glmark2.sh [-o OUT]" >&2; exit 2; }
command -v glmark2-wayland > /dev/null || { echo "glmark2: no glmark2-wayland" >&2; exit 1; }

runner_init glmark2 "$out"
runner_start
runner_record "case glmark2 the default benchmark list, validated and scored"
runner_record "warmup 0"
runner_record "repetitions 1 validation and 1 benchmark run"
runner_record "glmark2 $(glmark2-wayland --version 2>&1 | head -n 1)"
runner_weston
size=${RUNNER_WIDTH}x$RUNNER_HEIGHT

glmark2-wayland --off-screen -s "$size" --validate > "$out/validate.log" 2>&1 ||
    runner_die "glmark2-wayland --validate failed: $(tail -n 3 "$out/validate.log")"
sed -n 's/^ *\(\[[^]]*\]\) \(.*\): Validation: \([A-Za-z]*\).*/\1\2 \3/p' "$out/validate.log" |
    tr -d ' ' | sed 's/\(.*\)\(Success\|Failure\|Unknown\)$/\1 \2/' > "$out/validations"
runner_record "renderer $(sed -n 's/^ *GL_RENDERER: *//p' "$out/validate.log" | head -n 1)"
glmark2-wayland --off-screen -s "$size" > "$out/benchmark.log" 2>&1 ||
    runner_die "glmark2-wayland failed: $(tail -n 3 "$out/benchmark.log")"
sed -n 's/^ *\(\[[^]]*\]\) \(.*\): FPS: \([0-9]*\).*/\1\2 \3/p' "$out/benchmark.log" |
    sed 's/ \([0-9]*\)$/@\1/' | tr -d ' ' | tr '@' ' ' > "$out/scores"
score=$(sed -n 's/.*glmark2 Score: *\([0-9]*\).*/\1/p' "$out/benchmark.log" | tail -n 1)
echo "score ${score:-none}" >> "$out/scores"
runner_finish
[ -s "$out/validations" ] && [ -n "$score" ] ||
    runner_die "glmark2-wayland gave no validations or no score"
count()
{
    grep -c " $1\$" "$out/validations" || true
}
echo "glmark2: result: $(wc -l < "$out/validations") scenes: Success $(count Success)" \
    "Failure $(count Failure) Unknown $(count Unknown) score $score" | tee -a "$out/results"
