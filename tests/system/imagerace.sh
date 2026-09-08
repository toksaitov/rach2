#!/usr/bin/env sh
#
# tests/system/imagerace.sh: two builds of one image at the same moment, and
# the reason two builds meet.
#
# `tests/run` builds the shared images once before its pool, but a suite in
# the pool can still find one out of date and rebuild it, and two suites can
# reach that point together. With one fixed name for each temporary, the
# second build removed the first's file, failed, and make deleted the image
# both of them needed: on 2026-09-04 a full run lost the dhcp and procfiles
# suites that way, three seconds each, while every other suite passed. The
# temporaries carry the builder's process number now, so both builds finish
# and the image is whole; on 2026-09-09 the installer and installer-repair
# suites went down the same way on the sealed disk and the install stick,
# whose builders still had one fixed name each.
#
# The suites meet at all because an image is out of date the moment it is
# built: a manifest's `tree` line stages a source directory, `manifest_deps.sh`
# printed that directory as a prerequisite, and the Makefile has a phony
# target of the same name, which make remakes every time. So the last two
# checks are of the prerequisites themselves.
#
# Usage: imagerace.sh MANIFEST OUT SIZE_MB TEST_IMAGE OS_REAL_IMAGE
set -u
MANIFEST=$1; OUT=$2; SIZE=$3; TESTIMG=$4; REALIMG=$5
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
else
    exit 1
fi

# Two suites reach one image at once only because the image is out of date
# after it has just been built, and it was: every path a manifest stages is a
# prerequisite of the image, and a `tree` line names a source directory whose
# name is also a phony target, which make remakes every time. Both halves are
# checked here: what the script prints, and what make then does with it.
for m in krffs/manifests/*.manifest; do
    dirs=""
    for p in $(sh krffs/manifest_deps.sh "$m"); do
        [ -d "$p" ] && dirs="$dirs $p"
    done
    [ -z "$dirs" ] || bad "$m stages a directory as a prerequisite:$dirs"
done
[ "$fails" = 0 ] && say "every path a manifest stages is a file: OK"

# The images this suite's own target has built. `make -n` names the builder of
# anything out of date; the order-only components print their own lines every
# time, so what is looked for is a recipe that writes one of these.
for t in "$TESTIMG" "$REALIMG"; do
    stale=$(MAKEFLAGS= make --no-print-directory -n "$t" 2>&1 |
            grep -E "mkimage\.sh|mkext2\.sh|mkdisk_grub\.sh|mkinstallstick\.sh" |
            grep -c "$t")
    [ "$stale" = 0 ] || bad "$t is out of date the moment after it was built"
done
[ "$fails" = 0 ] || exit 1
say "an image just built is not out of date: OK"

# The shape of the fault of 2026-09-09: a suite writes a fixture below a staged
# `exec-dir` while the set runs. The stager takes the executables of that one
# directory and nothing beneath it, so a file in a directory below it may not
# put the image out of date. The directory itself stays a prerequisite through
# the image's `.d` file, since a program that appears in it is staged, so the
# marker goes one level down and not beside the programs.
mkdir -p build/tests/system/imagerace.d
: > build/tests/system/imagerace.d/marker
stale=$(MAKEFLAGS= make --no-print-directory -n "$TESTIMG" 2>&1 |
        grep -E "mkimage\.sh" | grep -c "$TESTIMG")
rm -rf build/tests/system/imagerace.d
[ "$stale" = 0 ] || bad "$TESTIMG went out of date for a file written below a staged directory"
[ "$fails" = 0 ] || exit 1
say "a file written below a staged directory does not put the image out of date: OK"
say "PASS"
