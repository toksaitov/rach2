#!/bin/sh
# probe.sh: the speed gate's WebGL fill and ALU checks in a browser, inside a
# graphics guest or on the Lenovo, Rach2's or Debian's, as an ordinary user.
#
#   probe.sh [-b BROWSER] [-o OUT] [-t | -r DRAW]
#
# Opens probe.html in BROWSER (chromium; or firefox) under the runners'
# compositor (common.sh), with a new profile, and reads what it posts. For a
# WebGL 1 and then a WebGL 2 context on a 1024x1024 canvas, the page times
# two draws, each the median of five runs after one to warm up, the GPU
# waited for by reading one pixel back before and after: the fill, 200
# quads over the whole canvas, blended, given in megapixels a second; and
# the ALU loop, 4 quads over the whole canvas whose every fragment runs 512
# dependent steps of a multiply-add and a sine, given in milliseconds: the
# two kinds of check that step 9 of the second discovery run measured on the
# Lenovo, a memory-bound fill and a loop in registers. Linux's values for
# the gate come from this page under Debian on the same machine.
# A browser that posts nothing for 120 s has stopped and is ended; the clock
# is the measure itself and decides nothing else.
#
# OUT (.) gets the record and `measures`, `KEY VALUE` a line, the form
# `judge.py gate` reads: webgl1-fill-BROWSER and webgl2-fill-BROWSER in
# Mpix/s, webgl1-alu-BROWSER and webgl2-alu-BROWSER in ms. It prints, for
# each context that gave both measures, the milliseconds of each timed run
# in the order they ran, which tell a slow stretch of a few runs from a
# draw that is slow every time, then the result:
#
#   probe: rounds: BROWSER webglN fill MS... alu MS...
#   probe: result: BROWSER webgl1 fill F1 alu A1 webgl2 fill F2 alu A2
#
# Each probe event in `events` has three times per measured round in
# fill_stamps_ms and alu_stamps_ms: the start, the return from draw and
# the return from readPixels. They are milliseconds since time_origin_ms,
# the browser's epoch timestamp. The page sends them with the result.
#
# -t has server.py snapshot /proc's schedstat and stat of every thread on
# the machine, and /proc/stat's time of each processor, before the first and
# after the last of each draw's six runs. A snapshot falls outside what the
# page times but adds some tens of milliseconds between the draws, which
# moves each draw against whatever else the machine runs then, so the gate
# runs without them. OUT then also gets `threads`, `webglN DRAW
# TID<TAB>PROCESS<TAB>THREAD<TAB>run MS<TAB>wait MS<TAB>user MS<TAB>system
# MS<TAB>runs N<TAB>HOW` for every thread that ran or waited for a processor
# in a draw's window, busiest first: its time on a processor and waiting
# for one by schedstat, its user and system time by stat, in whole clock
# ticks, and the times it was put on a processor; HOW is `nice N` when the
# thread's nice value is not 0, and `batch`, `idle` or `policy N` when its
# policy is not SCHED_OTHER, as the last snapshot found them. Before the
# result it prints for each draw its window's milliseconds, the run, wait,
# user and system time of all threads in it, each processor's busy and idle
# milliseconds, and the eight busiest threads, which tell a draw that waited
# for a processor from one that waited in the kernel or on the GPU, and a
# processor left idle while threads waited from one that was full:
#
#   probe: threads: BROWSER webglN DRAW MS ms, all ran MS ms and waited
#       MS ms, user MS ms and system MS ms; cpu N busy MS idle MS, ...;
#       PROCESS/THREAD run MS wait MS user MS system MS runs N [HOW], ...
#
# -r DRAW brackets each measured round of webgl1-fill, webgl1-alu,
# webgl2-fill or webgl2-alu with snapshots in `events`. The marks also
# carry Rach2's bkl/calls and the monotonic time of each read when debugfs
# is mounted. The marks run outside the browser's timing, but change the
# spacing of the rounds. Use a separate run without -r as the control.
#
# The status is 0 when both contexts gave both measures and 1 otherwise.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

browser=chromium out=. threads='' rounds=''
while getopts b:o:tr: o; do
    case $o in
        b) browser=$OPTARG ;;
        o) out=$OPTARG ;;
        t) threads=1 ;;
        r) rounds=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -ne 0 ] || { [ -n "$threads" ] && [ -n "$rounds" ]; }; then
    echo "usage: probe.sh [-b BROWSER] [-o OUT] [-t | -r DRAW]" >&2
    exit 2
fi
case $browser in chromium | firefox) ;; *) echo "probe: no browser $browser" >&2; exit 2 ;; esac
query=
case $rounds in
    webgl1-fill | webgl1-alu | webgl2-fill | webgl2-alu) query="?rounds=$rounds" ;;
    '') [ -z "$threads" ] || query='?threads' ;;
    *) echo "probe: no draw $rounds" >&2; exit 2 ;;
esac

runner_init probe "$out"
runner_start
runner_record "case probe $browser webgl1 and webgl2 fill and alu"
runner_record "warmup 1 of each draw"
runner_record "repetitions 5 of each draw, the median kept"
[ -z "$threads" ] || runner_record "threads snapshot around each draw"
[ -z "$rounds" ] || runner_record "threads and bkl calls around each $rounds round"
runner_browser_record "$browser"
runner_weston
runner_server "$RUNNER_DIR"
runner_browser "$browser" "http://127.0.0.1:$RUNNER_PORT/probe.html$query" "$RUNNER_OUT/browser.log"
# shellcheck disable=SC2046 # two words
set -- $(python3 -I "$RUNNER_DIR/events.py" wait "$RUNNER_WORK/events" 0 "$RUNNER_BROWSER" 120)
runner_stop
runner_record "browser $1 after $2 events"
cp "$RUNNER_WORK/events" "$RUNNER_OUT/events"
runner_finish
python3 -I - "$RUNNER_OUT/events" "$browser" "$RUNNER_OUT/measures" "$RUNNER_OUT/record" \
    "${threads:+$RUNNER_OUT/threads}" << 'EOF'
import json, sys
events, browser, measures, record, threads = sys.argv[1:6]
found, marks = {}, {}
with open(events, encoding="utf-8") as f:
    for line in f:
        e = json.loads(line)
        if e.get("kind") == "probe":
            found[e["api"]] = e
        elif e.get("kind") == "mark" and "round" not in e and isinstance(e.get("threads"), dict):
            marks[(e.get("api"), e.get("draw"), e.get("at"))] = e
rows, words, ok = [], [], True
with open(record, "a", encoding="utf-8") as rec:
    for api in (1, 2):
        e = found.get(api, {"error": "no result"})
        rec.write("renderer webgl%d %s\n" % (api, e.get("renderer", "-")))
        if e.get("error") or "fill_mpix" not in e or "alu_ms" not in e:
            rec.write("error webgl%d %s\n" % (api, e.get("error") or "no measure"))
            ok = False
            words.append("webgl%d %s" % (api, e.get("error") or "none"))
            continue
        if "fill_rounds" in e and "alu_rounds" in e:
            print("probe: rounds: %s webgl%d fill %s alu %s" % (
                browser, api, " ".join("%.1f" % t for t in e["fill_rounds"]),
                " ".join("%.1f" % t for t in e["alu_rounds"])))
        rows.append("webgl%d-fill-%s %.1f" % (api, browser, e["fill_mpix"]))
        rows.append("webgl%d-alu-%s %.2f" % (api, browser, e["alu_ms"]))
        words.append("webgl%d fill %.1f alu %.2f" % (api, e["fill_mpix"], e["alu_ms"]))
# Each draw's window, from the end of its first snapshot to the start of its
# last: the run and wait of every thread that ran or waited in it, busiest
# first, into `threads`, and the busiest few printed; with -t alone.
if threads:
    with open(threads, "w", encoding="utf-8") as tf:
        for api in (1, 2):
            for draw in ("fill", "alu"):
                a, b = marks.get((api, draw, "start")), marks.get((api, draw, "end"))
                if not a or not b:
                    continue
                wall = 1000 * (b["snap"][0] - a["snap"][1])
                rows_t = []
                for tid, (pname, name, run, wait, nice, policy, user, system, runs) in b["threads"].items():
                    run0, wait0, _, _, user0, system0, runs0 = a["threads"].get(tid, [0] * 9)[2:9]
                    if (run - run0) or (wait - wait0):
                        how = ("" if not nice else " nice %d" % nice) + \
                            {0: "", 3: " batch", 5: " idle"}.get(policy, " policy %d" % policy)
                        rows_t.append(((run - run0) / 1e6, (wait - wait0) / 1e6, tid, pname, name, how,
                                       user - user0, system - system0, runs - runs0))
                rows_t.sort(key=lambda r: -(r[0] + r[1]))
                for r in rows_t:
                    tf.write("webgl%d %s %s\t%s\t%s\trun %.1f\twait %.1f\tuser %d\tsystem %d\truns %d\t%s\n"
                             % (api, draw, r[2], r[3], r[4], r[0], r[1], r[6], r[7], r[8], r[5].strip()))
                cpus = ", ".join("cpu %s busy %d idle %d" % (n, b["cpus"][n][0] - a["cpus"][n][0],
                                                            b["cpus"][n][1] - a["cpus"][n][1])
                                 for n in sorted(b.get("cpus", {}), key=int) if n in a.get("cpus", {}))
                print("probe: threads: %s webgl%d %s %.0f ms, all ran %.0f ms and waited %.0f ms, "
                      "user %d ms and system %d ms; %s; %s" % (
                          browser, api, draw, wall, sum(r[0] for r in rows_t), sum(r[1] for r in rows_t),
                          sum(r[6] for r in rows_t), sum(r[7] for r in rows_t), cpus,
                          ", ".join("%s/%s run %.0f wait %.0f user %d system %d runs %d%s"
                                    % (r[3], r[4], r[0], r[1], r[6], r[7], r[8], r[5]) for r in rows_t[:8])))
with open(measures, "w", encoding="utf-8") as f:
    f.write("".join(r + "\n" for r in rows))
print("probe: result: %s %s" % (browser, " ".join(words)))
sys.exit(0 if ok else 1)
EOF
