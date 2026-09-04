#!/usr/bin/env sh
#
# tests/system/imagerace.sh: two builds of one image at the same moment.
#
# `tests/run` builds the shared images once before its pool, but a suite in
# the pool can still find one out of date and rebuild it, and two suites can
# reach that point together. With one fixed name for each temporary, the
# second build removed the first's file, failed, and make deleted the image
# both of them needed: on 2026-09-04 a full run lost the dhcp and procfiles
# suites that way, three seconds each, while every other suite passed. The
# temporaries carry the builder's process number now, so both builds finish
# and the image is whole.
#
# Usage: imagerace.sh MANIFEST OUT SIZE_MB
set -u
MANIFEST=$1; OUT=$2; SIZE=$3
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
fails=0
say() { echo "imagerace-test: $1"; }
bad() { echo "imagerace-test: FAIL: $1" >&2; fails=$((fails + 1)); }

rm -f "$OUT" "$OUT.d"
sh krffs/mkimage.sh "$MANIFEST" "$OUT" "$SIZE" > "$OUT.a.log" 2>&1 &
a=$!
sh krffs/mkimage.sh "$MANIFEST" "$OUT" "$SIZE" > "$OUT.b.log" 2>&1 &
b=$!
wait $a; rca=$?
wait $b; rcb=$?
[ "$rca" = 0 ] || bad "the first build failed: $(tail -2 "$OUT.a.log" | tr '\n' ' ')"
[ "$rcb" = 0 ] || bad "the second build failed: $(tail -2 "$OUT.b.log" | tr '\n' ' ')"
[ -s "$OUT" ] || bad "no image was left behind"
[ -s "$OUT.d" ] || bad "no dependency file was left behind"
# Nothing of either build is left in the way of the next one.
left=$(ls "$OUT".*.new* "$OUT".d.list* 2>/dev/null | tr '\n' ' ')
[ -z "$left" ] || bad "temporaries left behind: $left"
if [ "$fails" = 0 ]; then
    say "two builds of one image at once both finish and leave it whole: OK"
    say "PASS"
else
    exit 1
fi
