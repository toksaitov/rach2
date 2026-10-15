#!/usr/bin/env sh
#
# tests/system/imagerace.sh: two builds of one image at the same moment, and
# the reasons two builds would meet.
#
# `tests/run` builds the shared images once before its pool, but a suite in
# the pool that finds one out of date rebuilds it, and two suites can reach
# that point together. Each builder names its temporaries with its process
# number, so two builds of one image both finish and leave it whole; with
# one fixed name, the second build would remove the first's file and fail,
# and make would delete the image.
#
# The suites would meet only if an image were out of date the moment it is
# built: if a prerequisite were a directory with the name of a phony target,
# which make remakes every time, or if a file written below a staged
# directory changed the image's prerequisites. The later checks are of the
# prerequisites themselves, and of the rule that a changed program restages
# its image.
#
# Usage: imagerace.sh MANIFEST OUT SIZE_MB TEST_IMAGE OS_REAL_IMAGE
set -u
MANIFEST=$1; OUT=$2; SIZE=$3; TESTIMG=$4; REALIMG=$5
# The architecture's build tree, build or build/x86_64, the one the images
# are in.
BUILD=${TESTIMG%/images/*}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO" || exit 1
fails=0
# say TEXT: print a progress line.
say() { echo "imagerace-test: $1"; }
# bad TEXT: report a failed check and count it in `fails`.
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
left=
for t in "$OUT".*.new* "$OUT".d.list*; do
    [ ! -e "$t" ] || left="$left $t"
done
[ -z "$left" ] || bad "temporaries left behind: $left"
if [ "$fails" = 0 ]; then
    say "two builds of one image at once both finish and leave it whole: OK"
else
    exit 1
fi

# Every path a manifest stages is a prerequisite of the image, and a
# directory among them, such as the source directory of a `tree` line, can
# have the name of a phony target, which make remakes every time. This check
# reads what manifest_deps.sh prints, and the next what make does with it.
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
    stale=$(MAKEFLAGS='' make --no-print-directory -n "$t" 2>&1 |
            grep -E "mkimage\.sh|mkext2\.sh|mkdisk_grub\.sh|mkinstallstick\.sh" |
            grep -c "$t")
    [ "$stale" = 0 ] || bad "$t is out of date the moment after it was built"
done
[ "$fails" = 0 ] || exit 1
say "an image just built is not out of date: OK"

# A suite may write a fixture below a staged `exec-dir` while the set runs. The
# stager takes the executables of that one directory and nothing beneath it, so
# a file in a directory below it may not put the image out of date. A file or a
# directory made beside the programs may not either, since the image depends on
# the programs the directory holds and not on its time (krffs/programs.sh); the
# tree below has those rows, so that this suite does not put the shared image
# out of date.
mkdir -p "$BUILD/tests/system/imagerace.d"
: > "$BUILD/tests/system/imagerace.d/marker"
stale=$(MAKEFLAGS='' make --no-print-directory -n "$TESTIMG" 2>&1 |
        grep -E "mkimage\.sh" | grep -c "$TESTIMG")
rm -rf "$BUILD/tests/system/imagerace.d"
[ "$stale" = 0 ] || bad "$TESTIMG went out of date for a file written below a staged directory"
[ "$fails" = 0 ] || exit 1
say "a file written below a staged directory does not put the image out of date: OK"

# A staged program rebuilt on a built tree must still restage its image,
# which is why the staged paths are prerequisites. `make -W` cannot ask that
# of the shared tree: a staged path has a rule of its own that waits for its
# component and makes make read the file's time again, and that second read
# takes back the time `-W` supplied. The tree's database is asked instead,
# for both halves of the arrangement: the image names the program among its
# prerequisites, and the program waits for the component that builds it. An
# edited probe reaching the image in one `make` is checked with a real build
# against a copy of these makefiles by imagerace_probe.sh at the end of this
# suite.
db=$(MAKEFLAGS='' make --no-print-directory -p -n "$TESTIMG" 2>/dev/null)
printf '%s\n' "$db" | grep -q "^$BUILD/tests/vmprobe: tests\$" ||
    bad "a staged program does not wait for the component that builds it"
printf '%s\n' "$db" | grep -q "^$TESTIMG:.* $BUILD/tests/vmprobe " ||
    bad "$TESTIMG does not name a program it stages among its prerequisites"
[ "$fails" = 0 ] || exit 1
say "an image names the programs it stages, and each waits for its component: OK"

# Source to image, end to end, in a tree of this suite's own: a program, a
# manifest that stages it, and a makefile with the image rule in the shape
# of the Makefile's, the manifest's paths from `manifest_deps.sh` as its
# prerequisites, the component's own make as an order-only one, the `.d`
# file `mkimage.sh` writes included. The source is changed so the program
# says something else, exactly the image's target is made, and the image
# must carry the new words and not the old; made again with nothing
# changed, it must not be rebuilt. Both dependency mechanisms have to be
# dropped for the image to go stale: the prerequisites say what the
# manifest stages by name, and the `.d` file says what the last build read.
TREE=build/tests/system/imagerace.tree
MANIFEST=krffs/manifests/imagerace-tree.manifest
rm -rf "$TREE"
mkdir -p "$TREE"
cat > "$TREE/prog.c" <<'EOP'
int main(void) { return 0; }
const char words[] = "IMAGERACE FIRST WORDS";
EOP
mkdir -p "$TREE/bin"
# The image tools run from the repository's root, as the Makefile does, so
# the tree's makefile is run from there with the tree's paths under build/.
# The program's rule is in a makefile of its own, as a component's is, so
# the image's makefile knows the program only by name, as the Makefile does.
cat > "$TREE/Makefile" <<'EOP'
IMG = $(TREE)/tree.krffs
$(IMG): $(MANIFEST) $(shell sh krffs/manifest_deps.sh $(MANIFEST)) | prog
	sh krffs/mkimage.sh $(MANIFEST) $@ 2
-include $(IMG).d
_ := $(shell sh krffs/programs.sh check $(wildcard $(IMG).programs))
prog:
	$(MAKE) --no-print-directory -f $(TREE)/prog.mk TREE=$(TREE) $(TREE)/bin/prog
.PHONY: prog
EOP
cat > "$TREE/prog.mk" <<'EOP'
$(TREE)/bin/prog: $(TREE)/prog.c
	cc -o $@ $(TREE)/prog.c
EOP
# treemake ARGS...: make in the private tree, with the output in make.log.
treemake() { MAKEFLAGS='' make --no-print-directory -f "$TREE/Makefile" TREE="$TREE" MANIFEST="$MANIFEST" "$@" > "$TREE/make.log" 2>&1; }
treemake "$TREE/tree.krffs" || bad "the tree's first build failed: $(tail -2 "$TREE/make.log" | tr '\n' ' ')"
grep -aq "IMAGERACE FIRST WORDS" "$TREE/tree.krffs" || bad "the first image does not carry the program's words"
sleep 1 # make judges by time, and the file systems here keep seconds
sed -i.bak 's/FIRST WORDS/SECOND WORDS/' "$TREE/prog.c"; rm -f "$TREE/prog.c.bak"
treemake "$TREE/tree.krffs" || bad "the tree's second build failed: $(tail -2 "$TREE/make.log" | tr '\n' ' ')"
grep -aq "IMAGERACE SECOND WORDS" "$TREE/tree.krffs" || bad "the image was not rebuilt for the changed source"
grep -aq "IMAGERACE FIRST WORDS" "$TREE/tree.krffs" && bad "the image still carries the old words"
treemake "$TREE/tree.krffs" || bad "the tree's third build failed"
grep -q "mkimage.sh" "$TREE/make.log" && bad "the image was rebuilt with nothing changed"
[ "$fails" = 0 ] || exit 1
say "a changed source reaches the image through exactly its target, and an unchanged one does not rebuild it: OK"

# The staged directory judged by its programs: a proof's work directory and a
# log made beside the program, which the stager does not take, leave the
# image as it was; a second program made there restages it, and so does its
# removal, once.
sleep 1
mkdir "$TREE/bin/work.proof"
: > "$TREE/bin/notes.log"
treemake "$TREE/tree.krffs" || bad "the build beside a work directory failed"
grep -q "mkimage.sh" "$TREE/make.log" && bad "a work directory and a log beside the program rebuilt the image"
cp "$TREE/bin/prog" "$TREE/bin/prog2"
treemake "$TREE/tree.krffs" || bad "the build with a second program failed"
grep -q "mkimage.sh" "$TREE/make.log" || bad "a second program beside the first did not rebuild the image"
rm -f "$TREE/bin/prog2"
treemake "$TREE/tree.krffs" || bad "the build after the second program went failed"
grep -q "mkimage.sh" "$TREE/make.log" || bad "the second program's removal did not rebuild the image"
treemake "$TREE/tree.krffs" || bad "the build after that failed"
grep -q "mkimage.sh" "$TREE/make.log" && bad "the image was rebuilt again with nothing changed"
[ "$fails" = 0 ] || exit 1
say "a staged directory is judged by its programs: a work directory beside them does not rebuild the image, a program made or removed does: OK"
sh tests/system/imagerace_probe.sh || exit 1
say "PASS"
