#!/bin/sh
#
# imagerace.sh: the program of the imagerace cases,
# tests/cases/host.imagerace.json: how krffs/mkimage.sh publishes an image
# when two builds of it run at once or one is stopped, and the prerequisites
# that keep an image current.
#
# `tests/run` builds the shared images once before its pool, but a suite in
# the pool that finds one out of date rebuilds it, and two suites can reach
# that point together. mkimage.sh names its temporaries with its process
# number and publishes the image last, after its dependency file and its
# list of programs, so two builds at once both finish and leave the image
# whole, and a build stopped at any step leaves a set that make either
# accepts as current or builds again. Two builds would meet only if an image
# were out of date the moment it is built, so the other cases check the
# prerequisites themselves, and that a changed program restages its image.
#
# Each run is one case, named by its argument: `concurrent`, `interrupted`,
# `prerequisites`, `restage` or `probe`. It writes the case's report
# records on its standard output, an assertion for each check
# (tools/etude/Readme.md, Report records), with the lines a person reads
# between them, and exits 0 when every assertion passed and 1 when one
# failed. The manifest says what each case and each assertion checks.
#
# etude runs it in the case's workspace, with ETUDE_TREE naming the root of
# the tree, to which it changes first, ARCH naming the architecture of a
# case that has one, and TMPDIR a scratch directory of the case's own. The
# cases but `prerequisites` and `probe` build an image of their own, in a
# private tree under TMPDIR, from a copy of
# krffs/manifests/imagerace-tree.manifest that stages the private tree's
# programs.
#
# Usage, in the root of a built tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/imagerace.sh CASE
#
# The private directory under TMPDIR, whose path the first line prints, is
# removed by a run that passes or is stopped by a signal, and kept by one
# that fails.
#

# shellcheck disable=SC2329 # the cases are called by name, at the end
set -u

CASE=${1-}
[ "$#" -eq 1 ] && [ -n "$CASE" ] || { echo "imagerace: usage: imagerace.sh CASE" >&2; exit 2; }
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
[ -f tests/registry ] && [ -f krffs/mkimage.sh ] || {
    echo "imagerace: run it in the root of the tree" >&2
    exit 2
}

# The exit trap keeps the private directory only after a failure, and
# removes the marker the prerequisites case writes into the build tree.
unset W MARK
status=0
trap 'status=$?; rm -rf ${MARK:+"$MARK"}; [ "$status" -ne 0 ] || rm -rf ${W:+"$W"}; exit "$status"' EXIT
trap 'rm -rf ${MARK:+"$MARK"} ${W:+"$W"}; exit 129' HUP
trap 'rm -rf ${MARK:+"$MARK"} ${W:+"$W"}; exit 130' INT
trap 'rm -rf ${MARK:+"$MARK"} ${W:+"$W"}; exit 143' TERM
if ! W=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/imagerace.XXXXXX") || ! W=$(cd "$W" && pwd -P); then
    echo "imagerace: cannot create a temporary directory" >&2
    exit 1
fi
printf 'imagerace: work: %s\n' "$W"

# ---- The report. ----

RECORDS=0
FAILED=""

# Write the report record WORDS, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# Make TEXT printable ASCII of at most 160 bytes with no space at either
# end, for a record's detail, which a space there would keep from parsing.
clip() {
    _clip=$(printf '%s' "$1" | tr -c ' -~' '?' | sed 's/^ *//' | cut -c1-160 | sed 's/ *$//')
    printf '%s' "${_clip:-?}"
}

# pass ID: report that the assertion ID passed.
pass() {
    record "assert $1 pass"
    RECORDS=$((RECORDS + 1))
}

# fail ID DETAIL: report that the assertion ID failed, with DETAIL.
fail() {
    record "assert $1 fail $(clip "$2")"
    RECORDS=$((RECORDS + 1))
    FAILED="$FAILED $1"
}

# judge ID DETAIL COMMAND...: report the assertion ID passed when COMMAND
# succeeds, and failed with DETAIL otherwise.
judge() {
    _id=$1
    _detail=$2
    shift 2
    if "$@"; then
        pass "$_id"
    else
        fail "$_id" "$_detail"
    fi
}

# broken DETAIL ID...: report each assertion ID failed with DETAIL, for a
# case whose setup failed before it could check them.
broken() {
    _detail=$1
    shift
    echo "imagerace: $_detail" >&2
    for _id in "$@"; do
        fail "$_id" "$_detail"
    done
}

# ---- The private tree. ----

# tree DIR: make the private tree DIR: a program's source and the makefile
# that builds it, as a component's is, and a makefile with the image rule in
# the shape of the root Makefile's: the manifest's paths from
# manifest_deps.sh as its prerequisites, the component's own make as an
# order-only one, each staged path waiting for the component with an empty
# recipe, so that make reads its time again after the component's make, and
# the `.d` file mkimage.sh writes included. The manifest
# is a copy of imagerace-tree.manifest that stages DIR/bin, and keeps the
# name the set-id allowlist knows it by. The image tools run from the root
# of the tree, as the Makefile does, so the private makefile is run from
# there with absolute paths.
tree() {
    mkdir -p "$1/bin" || return 1
    sed "s|^exec-dir 0755 / .*|exec-dir 0755 / $1/bin|" \
        krffs/manifests/imagerace-tree.manifest > "$1/imagerace-tree.manifest" || return 1
    cat > "$1/prog.c" <<'EOP' || return 1
int main(void) { return 0; }
const char words[] = "IMAGERACE FIRST WORDS";
EOP
    cat > "$1/Makefile" <<'EOP' || return 1
IMG = $(TREE)/tree.krffs
MANIFEST = $(TREE)/imagerace-tree.manifest
STAGED := $(shell sh krffs/manifest_deps.sh $(MANIFEST))
$(IMG): $(MANIFEST) $(STAGED) | prog
	sh krffs/mkimage.sh $(MANIFEST) $@ 2
$(STAGED): prog
	+@:
-include $(IMG).d
_ := $(shell sh krffs/programs.sh check $(wildcard $(IMG).programs))
prog:
	$(MAKE) --no-print-directory -f $(TREE)/prog.mk TREE=$(TREE) $(TREE)/bin/prog
.PHONY: prog
EOP
    cat > "$1/prog.mk" <<'EOP'
$(TREE)/bin/prog: $(TREE)/prog.c
	cc -o $@ $(TREE)/prog.c
EOP
}

# treemake DIR ARGS...: make ARGS in the private tree DIR, with the output
# in DIR/make.log.
treemake() {
    _t=$1
    shift
    MAKEFLAGS='' make --no-print-directory -f "$_t/Makefile" TREE="$_t" "$@" > "$_t/make.log" 2>&1
}

# rebuilt DIR: whether the last make in DIR staged the image.
rebuilt() {
    grep -q "mkimage.sh" "$1/make.log"
}

# older DIR: give every file of DIR one time long past. make judges by time,
# and a file system may keep only whole seconds, so this makes the next file
# written newer than all of them, however soon it is written; files of equal
# time are current to make.
older() {
    find "$1" -exec touch -t 200001010000 {} +
}

# leftovers IMAGE: print the names beside IMAGE that are neither it, its
# dependency file nor its list of programs: the temporaries of a build.
leftovers() {
    for _f in "$1".*; do
        [ -e "$_f" ] || continue
        case "$_f" in
            "$1.d" | "$1.programs") ;;
            *) printf ' %s' "${_f##*/}" ;;
        esac
    done
}

# shims DIR COMMAND...: put in DIR a shim of each COMMAND, which runs step.sh
# and then the command it stands for. A build run with DIR first on its path
# runs its steps through them.
shims() {
    _dir=$1
    shift
    mkdir -p "$_dir" || return 1
    cat > "$_dir/step.sh" <<'EOS' || return 1
# step.sh: sourced by a shim before the command it stands for. A command
# whose arguments name the image under test, IMAGERACE_OUT, is a step of its
# build. With IMAGERACE_STEPS set, the step takes the next number there, and
# the step whose number is IMAGERACE_STOP sends SIGTERM to the build, whose
# process ID is in IMAGERACE_STEPS/pid. With IMAGERACE_MEET set, the two
# builds, IMAGERACE_BUILD `a` and `b`, meet at the touch of the temporary
# image: each writes `met` into the other's FIFO there and reads a line from
# its own. The case holds both FIFOs open, and writes `alone` into the
# other's once a build has ended, so that a build is not held for one that
# failed.
case " $* " in
    *"${IMAGERACE_OUT:?}"*)
        if [ -n "${IMAGERACE_STEPS:-}" ]; then
            step_n=1
            while ! mkdir "$IMAGERACE_STEPS/$step_n" 2>/dev/null; do
                step_n=$((step_n + 1))
            done
            printf '%s\n' "${0##*/} $*" > "$IMAGERACE_STEPS/$step_n/command"
            if [ "$step_n" = "${IMAGERACE_STOP:-0}" ] &&
                kill -TERM "$(cat "$IMAGERACE_STEPS/pid")"; then
                : > "$IMAGERACE_STEPS/$step_n/stopped"
            fi
        fi
        if [ -n "${IMAGERACE_MEET:-}" ] && [ "${0##*/}" = touch ]; then
            case "$IMAGERACE_BUILD" in
                a) step_other=b ;;
                *) step_other=a ;;
            esac
            echo met > "$IMAGERACE_MEET/in.$step_other"
            read -r step_word < "$IMAGERACE_MEET/in.$IMAGERACE_BUILD"
            [ "$step_word" != met ] || : > "$IMAGERACE_MEET/met.$IMAGERACE_BUILD"
        fi
        ;;
esac
EOS
    for _c in "$@"; do
        _real=$(command -v "$_c") || return 1
        printf '#!/bin/sh\n. "%s/step.sh"\nexec "%s" "$@"\n' "$_dir" "$_real" > "$_dir/$_c" &&
            chmod 755 "$_dir/$_c" || return 1
    done
}

# ---- The cases. ----

# Two builds of one image at once, each held at the touch of its temporary
# image until the other has made its own, so that both temporaries exist
# together; with one fixed name, the second build would publish or remove
# the first's file. Each build writes its status and its name into the FIFO
# ends as it ends, so the case learns which ended first without a clock.
# The case holds the FIFOs open for reading and writing, which Linux and
# macOS allow, so that no open of them waits and no read meets an early end.
case_concurrent() {
    T=$W/tree
    IMG=$T/tree.krffs
    if ! { tree "$T" && treemake "$T" prog && shims "$W/shims" touch && mkdir "$W/meet" &&
           mkfifo "$W/meet/in.a" "$W/meet/in.b" "$W/meet/ends"; }; then
        broken "the private tree was not made; see $W" met first second image deps temporaries
        return
    fi
    exec 3<>"$W/meet/in.a" 4<>"$W/meet/in.b" 5<>"$W/meet/ends"
    for b in a b; do
        PATH="$W/shims:$PATH" IMAGERACE_OUT=$IMG IMAGERACE_MEET=$W/meet IMAGERACE_BUILD=$b \
            sh -c 'sh krffs/mkimage.sh "$@" > "$0.log" 2>&1; echo "$? ${0##*/}" > "${0%/*}/meet/ends"' \
            "$W/$b" "$T/imagerace-tree.manifest" "$IMG" 2 3>&- 4>&- 5>&- &
    done
    read -r s1 n1 <&5
    if [ "$n1" = a ]; then
        echo alone >&4
    else
        echo alone >&3
    fi
    read -r s2 n2 <&5
    wait
    exec 3>&- 4>&- 5>&-
    [ "$n1" = a ] && rca=$s1 || rca=$s2
    [ "$n2" = b ] && rcb=$s2 || rcb=$s1
    met=0
    for f in "$W"/meet/met.*; do
        [ ! -e "$f" ] || met=$((met + 1))
    done
    judge met "$met of the two builds met the other with its temporary image made" [ "$met" = 2 ]
    judge first "the first build failed: $(tail -2 "$W/a.log" | tr '\n' ' ')" [ "$rca" = 0 ]
    judge second "the second build failed: $(tail -2 "$W/b.log" | tr '\n' ' ')" [ "$rcb" = 0 ]
    judge image "no image with the program's words was left" grep -aqs "IMAGERACE FIRST WORDS" "$IMG"
    judge deps "no dependency file naming the program was left" grep -qsF "$T/bin/prog" "$IMG.d"
    left=$(leftovers "$IMG")
    judge temporaries "temporaries left:$left" [ -z "$left" ]
}

# stopped N: build the private tree's image again with the shims first on
# the path, counting its steps in W/steps.N and stopping it with SIGTERM at
# step N, or at none for 0; give the build's status.
stopped() {
    rm -rf "$W/steps.$1"
    mkdir "$W/steps.$1" || return 1
    PATH="$W/shims:$PATH" IMAGERACE_OUT=$IMG IMAGERACE_STEPS=$W/steps.$1 IMAGERACE_STOP=$1 \
        sh -c 'echo "$$" > "$IMAGERACE_STEPS/pid" && exec sh krffs/mkimage.sh "$@"' sh \
        "$T/imagerace-tree.manifest" "$IMG" 2 > "$W/steps.$1/log" 2>&1
}

# restore: put back the image, the dependency file and the list of programs
# of the build before the second program, with their times, and remove what
# a stopped build left beside them.
restore() {
    for _f in $(leftovers "$IMG"); do
        rm -f "$T/$_f"
    done
    cp -p "$W/old/tree.krffs" "$W/old/tree.krffs.d" "$W/old/tree.krffs.programs" "$T/"
}

# A rebuild that adds a second program, stopped by SIGTERM at each step of
# its publication in turn: the write of the temporary image, the reads of
# what the stager recorded, the moves of the list of programs, the
# dependency file and the image, and the touch before the last. Whatever
# the step, the build fails, leaves no temporary, and leaves no new image
# beside an old dependency file; and one make then gives what an
# uninterrupted build gives.
case_interrupted() {
    T=$W/tree
    IMG=$T/tree.krffs
    if ! { tree "$T" && treemake "$T" "$IMG" && mkdir "$W/old" "$W/new" &&
           cp -p "$IMG" "$IMG.d" "$IMG.programs" "$W/old/" &&
           printf '#!/bin/sh\necho IMAGERACE SECOND PROGRAM\n' > "$T/bin/prog2" &&
           chmod 755 "$T/bin/prog2" && shims "$W/shims" dd sed grep mv touch; }; then
        broken "the private tree was not made; see $W" counted status temporaries matched current
        return
    fi
    if ! stopped 0 || ! grep -aq "IMAGERACE SECOND PROGRAM" "$IMG"; then
        broken "the uninterrupted build failed; see $W/steps.0/log" counted status temporaries matched current
        return
    fi
    cp -p "$IMG" "$IMG.d" "$IMG.programs" "$W/new/"
    steps=0
    while [ -d "$W/steps.0/$((steps + 1))" ]; do
        steps=$((steps + 1))
    done
    echo "imagerace: the build has $steps steps:"
    sed 's/^/    /' "$W"/steps.0/*/command
    want=1
    for c in dd touch mv; do
        grep -q "^$c " "$W"/steps.0/*/command || want=0
    done
    [ "$(cat "$W"/steps.0/*/command | grep -c '^mv ')" -ge 3 ] || want=0
    judge counted "the build's steps lack the write, the touch or the three moves of its files" \
        [ "$want" = 1 ]
    bad_status=""
    bad_left=""
    bad_matched=""
    bad_current=""
    n=1
    while [ "$n" -le "$steps" ]; do
        restore
        stopped "$n"
        rc=$?
        [ -e "$W/steps.$n/$n/stopped" ] || bad_status="$bad_status $n(not-reached)"
        [ "$rc" != 0 ] || bad_status="$bad_status $n"
        left=$(leftovers "$IMG")
        [ -z "$left" ] || bad_left="$bad_left $n:$left"
        if grep -aq "IMAGERACE SECOND PROGRAM" "$IMG" &&
            ! { grep -qF "$T/bin/prog2" "$IMG.d" && grep -q ' prog2' "$IMG.programs"; }; then
            bad_matched="$bad_matched $n"
        fi
        if ! treemake "$T" "$IMG" || ! grep -aq "IMAGERACE FIRST WORDS" "$IMG" ||
            ! grep -aq "IMAGERACE SECOND PROGRAM" "$IMG" ||
            ! cmp -s "$IMG.d" "$W/new/tree.krffs.d" ||
            ! cmp -s "$IMG.programs" "$W/new/tree.krffs.programs"; then
            bad_current="$bad_current $n"
        fi
        n=$((n + 1))
    done
    judge status "builds stopped at these steps were not stopped, or ended with status 0:$bad_status" \
        [ -z "$bad_status" ]
    judge temporaries "builds stopped at these steps left temporaries:$bad_left" [ -z "$bad_left" ]
    judge matched "builds stopped at these steps left the new image beside old lists:$bad_matched" \
        [ -z "$bad_matched" ]
    judge current "after builds stopped at these steps, one make did not give the whole image:$bad_current" \
        [ -z "$bad_current" ]
}

# Every path a manifest stages is a prerequisite of its image, and a
# directory among them can have the name of a phony target, which make
# remakes every time; then the images just built, and the rule that a
# changed program restages its image. The checks ask the tree's own make,
# for the architecture ARCH, about the shared images its preparation built.
case_prerequisites() {
    case "${ARCH:-i686}" in
        x86_64) BUILD=build/x86_64 ;;
        *) BUILD=build ;;
    esac
    TESTIMG=$BUILD/images/test.krffs
    REALIMG=$BUILD/images/os-real.krffs

    # The paths as the root Makefile reads them, through its MANIFEST_DEPS,
    # which gives the architecture's path map. The rule is a makefile of its
    # own, since the make of macOS, 3.81, has no --eval.
    # shellcheck disable=SC2016 # the dollars are make's and its shell's
    echo 'imagerace-deps: ; @for m in krffs/manifests/*.manifest; do echo "== $$m"; $(MANIFEST_DEPS) "$$m"; done' \
        > "$W/deps.mk"
    if MAKEFLAGS='' make -s --no-print-directory -f Makefile -f "$W/deps.mk" \
        imagerace-deps > "$W/deps.log" 2>&1; then
        dirs=$(awk '/^== / { m = $2; next } m != "" { print m " " $0 }' "$W/deps.log" |
               while read -r m p; do [ ! -d "$p" ] || printf ' %s:%s' "${m##*/}" "$p"; done)
        judge files "a manifest stages a directory as a prerequisite:$dirs" [ -z "$dirs" ]
    else
        fail files "the paths of the manifests were not read; see $W/deps.log"
    fi

    # `make -n` names the builder of anything out of date; the order-only
    # components print their own lines every time, so what is looked for is
    # a recipe that writes one of the images.
    stale=""
    for t in "$TESTIMG" "$REALIMG"; do
        n=$(MAKEFLAGS='' make --no-print-directory -n "$t" 2>&1 |
            grep -E "mkimage\.sh|mkext2\.sh|mkdisk_grub\.sh|mkinstallstick\.sh" | grep -c "$t")
        [ "$n" = 0 ] || stale="$stale $t"
    done
    judge current "out of date the moment after they were built:$stale" [ -z "$stale" ]

    # A suite may write a fixture below a staged `exec-dir` while the set
    # runs. The stager takes the executables of that one directory and
    # nothing beneath it, so a file in a directory below may not put the
    # image out of date.
    MARK=$BUILD/tests/system/imagerace.$$.d
    if mkdir -p "$MARK" && : > "$MARK/marker"; then
        n=$(MAKEFLAGS='' make --no-print-directory -n "$TESTIMG" 2>&1 |
            grep -E "mkimage\.sh" | grep -c "$TESTIMG")
        judge beneath "$TESTIMG went out of date for a file written below a staged directory" [ "$n" = 0 ]
    else
        fail beneath "$MARK could not be made"
    fi
    rm -rf "$MARK"
    MARK=""

    # The tree's database, for both halves of the rule that a rebuilt
    # program restages its image: the image names the program among its
    # prerequisites, and the program waits for the component that builds
    # it. `make -W` cannot ask that of the shared tree, since a staged path
    # has a rule of its own that makes make read the file's time again.
    MAKEFLAGS='' make --no-print-directory -p -n "$TESTIMG" > "$W/db.log" 2>/dev/null
    judge named "$TESTIMG does not name a program it stages among its prerequisites" \
        grep -q "^$TESTIMG:.* $BUILD/tests/vmprobe " "$W/db.log"
    judge waits "a staged program does not wait for the component that builds it" \
        grep -q "^$BUILD/tests/vmprobe: tests\$" "$W/db.log"
}

# Source to image, end to end, in the private tree: the source is changed so
# the program says something else, exactly the image's target is made, and
# the image must carry the new words and not the old; made again with
# nothing changed, it must not be rebuilt. Both dependency mechanisms have
# to be dropped for the image to go stale: the prerequisites say what the
# manifest stages by name, and the `.d` file says what the last build read.
# Then the staged directory is judged by its programs: a work directory and
# a log made beside the program leave the image as it was, and a second
# program made there restages it, and so does its removal, once.
case_restage() {
    T=$W/tree
    IMG=$T/tree.krffs
    if ! tree "$T" || ! treemake "$T" "$IMG"; then
        broken "the private tree's first build failed; see $T/make.log" \
            first changed unchanged beside added removed settled
        return
    fi
    judge first "the first image does not carry the program's words" \
        grep -aq "IMAGERACE FIRST WORDS" "$IMG"
    older "$T"
    sed 's/FIRST WORDS/SECOND WORDS/' "$T/prog.c" > "$T/prog.c.new" && mv "$T/prog.c.new" "$T/prog.c"
    if treemake "$T" "$IMG" && grep -aq "IMAGERACE SECOND WORDS" "$IMG" &&
        ! grep -aq "IMAGERACE FIRST WORDS" "$IMG"; then
        pass changed
    else
        fail changed "the image made for the changed source does not carry its words alone; see $T/make.log"
    fi
    treemake "$T" "$IMG" && ! rebuilt "$T"
    judge unchanged "the image was rebuilt with nothing changed" [ "$?" = 0 ]

    older "$T"
    mkdir "$T/bin/work.proof"
    : > "$T/bin/notes.log"
    treemake "$T" "$IMG" && ! rebuilt "$T"
    judge beside "a work directory and a log beside the program rebuilt the image" [ "$?" = 0 ]
    cp "$T/bin/prog" "$T/bin/prog2"
    treemake "$T" "$IMG" && rebuilt "$T"
    judge added "a second program beside the first did not rebuild the image" [ "$?" = 0 ]
    older "$T"
    rm -f "$T/bin/prog2"
    treemake "$T" "$IMG" && rebuilt "$T"
    judge removed "the second program's removal did not rebuild the image" [ "$?" = 0 ]
    treemake "$T" "$IMG" && ! rebuilt "$T"
    judge settled "the image was rebuilt again with nothing changed" [ "$?" = 0 ]
}

# An edited probe reaches the test image in one make, in a private copy of
# the root Makefile and the makefiles it reads (imagerace_probe.sh).
case_probe() {
    if sh tests/system/imagerace_probe.sh > "$W/probe.log" 2>&1; then
        pass staged
    else
        fail staged "$(tail -1 "$W/probe.log")"
        sed -n '1,40p' "$W/probe.log" | sed 's/^/    /'
    fi
}

# ---- The run. ----

ID=host.imagerace.$CASE
if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "imagerace: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
    exit 2
fi
case "$CASE" in
    concurrent|interrupted|prerequisites|restage|probe) ;;
    *) echo "imagerace: no case $CASE" >&2; exit 2 ;;
esac
record "begin $ID"
"case_$CASE"
record "end $RECORDS"
if [ -z "$FAILED" ]; then
    echo "imagerace: result: passed: $ID"
    exit 0
fi
echo "imagerace: result: failed: $ID:$FAILED"
exit 1
