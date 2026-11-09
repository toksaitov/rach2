#!/bin/sh
# igt.sh: a part of IGT's tests by igt_runner, inside a graphics guest,
# Rach2's or Debian's, as root.
#
#   igt.sh [-l LIST] [-t ROOT] [-o OUT] [TEST]...
#
# Runs the lines of LIST (../igt-76.list) for each test program TEST, or
# every line without one, through igt_runner on the tests in ROOT
# (/usr/libexec/igt-gpu-tools). A line's pattern is expanded against
# ROOT/subtest-list.txt, the stage's list of every subtest, so that both
# guests run the same subtests by name. igt_runner runs each test program
# once for each of its subtests, as root on the card the test opens, and
# watches the kernel's log; igt.sh sets no time limit of its own.
#
# OUT (.) gets the record, `list`, the subtests asked for, `results.json`,
# igt_runner's results whole, and TEST.results for each test run, one line
# a result in the order of their names: `NAME RESULT`, NAME being
# igt@TEST@SUBTEST, with a fourth part for each dynamic subtest, and RESULT
# igt_runner's: pass, skip, fail, crash, timeout, warn, dmesg-warn,
# dmesg-fail, incomplete, abort or notrun. It prints one line:
#
#   igt: result: TESTS tests, SUBTESTS results: pass P skip S fail F other O
#
# The status is 0 when igt_runner made its results and 1 otherwise; a
# subtest's failure is a result, which judge.py compares with the
# reference's.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

list=$RUNNER_DIR/../igt-76.list root=/usr/libexec/igt-gpu-tools out=.
while getopts l:t:o: o; do
    case $o in
        l) list=$OPTARG ;;
        t) root=$OPTARG ;;
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ -f "$list" ] || { echo "igt: no list $list" >&2; exit 1; }
[ -f "$root/subtest-list.txt" ] || { echo "igt: no IGT stage at $root" >&2; exit 1; }
command -v igt_runner > /dev/null || { echo "igt: no igt_runner" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "igt: igt_runner runs the tests as root" >&2; exit 1; }

runner_init igt "$out"
runner_start
runner_record "case igt ${*:-all} from $(basename "$list") sha256 $(sha256sum < "$list" | cut -d' ' -f1)"
runner_record "warmup 0"
runner_record "repetitions 1"
runner_record "igt $root, $(wc -l < "$root/subtest-list.txt") subtests in its list"
runner_record "cards $(cd /dev/dri 2> /dev/null && echo card*)"
for name in /sys/kernel/debug/dri/[0-9]*/name; do
    [ -r "$name" ] && runner_record "driver $(cat "$name")"
done

# The list's lines for the tests asked for, each pattern expanded to the
# subtests it matches, in the stage list's order.
grep -v '^#' "$list" | grep . | while read -r line; do
    test=${line#igt@}
    test=${test%%@*}
    if [ $# -gt 0 ]; then
        case " $* " in *" $test "*) ;; *) continue ;; esac
    fi
    case $line in
        igt@*@*)
            pattern=${line#igt@"$test"@}
            matched=0
            while read -r name; do
                # shellcheck disable=SC2254 # the pattern is the list's
                case ${name#igt@"$test"@} in
                    $pattern) echo "$name"; matched=1 ;;
                esac
            done << EOF
$(grep "^igt@$test@" "$root/subtest-list.txt")
EOF
            [ "$matched" = 1 ] || echo "igt: $line matches no subtest" >&2
            ;;
        *)
            grep -q "^igt@$test@" "$root/subtest-list.txt" || echo "igt: $line names no test" >&2
            echo "$line"
            ;;
    esac
done > "$out/list"
[ -s "$out/list" ] || runner_die "the list has no line for ${*:-any test}"
runner_record "list $(wc -l < "$out/list") lines sha256 $(sha256sum < "$out/list" | cut -d' ' -f1)"

status=0
igt_runner -o --test-list "$out/list" "$root" "$RUNNER_WORK/results" \
    > "$out/igt_runner.log" 2>&1 || status=$?
runner_record "igt_runner status $status"
runner_finish
[ -f "$RUNNER_WORK/results/results.json" ] ||
    runner_die "igt_runner made no results, status $status: $(tail -n 3 "$out/igt_runner.log")"
cp "$RUNNER_WORK/results/results.json" "$out/results.json"

python3 -I - "$out/results.json" "$out" << 'EOF' | tee -a "$out/results"
import json, sys
tests = json.load(open(sys.argv[1], encoding="utf-8"))["tests"]
by_test = {}
for name, t in tests.items():
    by_test.setdefault(name.split("@")[1], []).append((name, t["result"]))
counts = {"pass": 0, "skip": 0, "fail": 0}
other = 0
for test, rows in sorted(by_test.items()):
    with open("%s/%s.results" % (sys.argv[2], test.replace("/", "-")), "w", encoding="utf-8") as f:
        for name, result in sorted(rows):
            f.write("%s %s\n" % (name, result))
            if result in counts:
                counts[result] += 1
            else:
                other += 1
print("igt: result: %d tests, %d results: pass %d skip %d fail %d other %d" %
      (len(by_test), len(tests), counts["pass"], counts["skip"], counts["fail"], other))
EOF
