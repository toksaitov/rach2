#!/bin/sh
# testbed.sh: one scene of the test bed's harness (bed/measure) on the
# machine's own GPU, the rung native-rach2, set beside Linux's on the same
# machine, as the native cases testbed/* of gpu.cases run it on the Lenovo.
#
#   testbed.sh SCENE LINUX
#
# Run as root by etude-guest in the case's workspace, which holds the
# harness, measure.sh, piglit.txt, latency.c and webgl.html, and the runners
# it uses, common.sh, games.sh and piglit.sh, with server.py and events.py
# for the webgl scene's page and its events. The script lays them out as
# measure.sh expects, a directory with the harness and its runners apart,
# and runs `measure.sh -r native-rach2 -u desktop SCENE` under a headless
# Weston of the harness's own, as Linux's native-linux lines were taken.
# LINUX, bed/measure/ref/lenovo-linux.lines, has Linux's line for each
# scene; the script prints Linux's line for SCENE beside Rach2's, or says
# that it has none yet. The case passes when the harness ran the scene to
# its end and printed its line; the figures are measured, not judged, as
# 83-7's report sets them side by side. The last line is `testbed: result:
# pass` or `fail: why`.
set -eu

[ $# -eq 2 ] || {
    echo "usage: testbed.sh glmark2|piglit|game|webgl LINUX" >&2
    exit 2
}
scene=$1
linux=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
here=$(pwd)
rm -rf harness out
mkdir -p harness/runners
for f in measure.sh piglit.txt latency.c webgl.html; do
    cp "$f" harness/
done
for f in common.sh games.sh piglit.sh; do
    cp "$f" harness/runners/
done
if [ "$scene" = webgl ]; then
    cp server.py events.py harness/runners/
fi
chmod -R a+rX harness
status=0
MEASURE_RUNNERS=$here/harness/runners sh harness/measure.sh -r native-rach2 -u desktop \
    -o "$here/out" "$scene" > measure.out 2>&1 || status=$?
cat measure.out
line=$(grep '^measure: result: ' measure.out | tail -n 1) || line=''
ref=$(grep "^measure: result: $scene native-linux " "$linux" | tail -n 1) || ref=''
echo "testbed: rach2: ${line:-none}"
echo "testbed: linux: ${ref:-none yet}"
if [ "$status" -ne 0 ] || [ -z "$line" ]; then
    echo "testbed: result: fail: measure.sh exited $status: $(tail -n 1 measure.out)"
    exit 1
fi
echo "testbed: result: pass"
