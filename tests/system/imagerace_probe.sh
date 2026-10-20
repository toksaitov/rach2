#!/bin/sh
# imagerace_probe.sh: check that an edited probe reaches the test image in
# one make, in a private copy of the root Makefile, the makefiles it includes
# from mk/ and tools/etude/, and the makefiles of tests/ and krffs/. The
# tree is i686's whatever ARCH the run that starts it is for: neither the
# architecture, in the environment or in the make flags tests/run gives it,
# nor the path map an x86-64 make exports reaches the tree.
#
# The image stages conloadprobe alone. After one make it must have the
# probe's bytes; after an edit a second later, one make must rebuild the
# probe and restage it; a third make and a dry run must not rebuild the
# image. IMAGERACE_MAKEFILE, when set, names the root Makefile to copy in
# place of the tree's; tests/proofs/imagerace.json runs faulty copies so.
set -eu
unset ARCH RACH2_ARCH RACH2_PATH_MAP MAKEFLAGS MFLAGS

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" imagerace-probe "$repo/tests/system/imagerace_probe.sh" "$@"
cd "$repo"
[ "$#" = 0 ] || { echo 'imagerace-probe: usage: imagerace_probe.sh' >&2; exit 2; }
makefile=${IMAGERACE_MAKEFILE:-$repo/Makefile}

mkdir -p build/tests/system
work=
logged=
child=

# Stop the build and remove the scratch directory. A failure after its path
# was printed keeps it as evidence; a signal does not.
cleanup()
{
    status=$?
    [ -n "$work" ] || return 0
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    case $status in
        0|129|130|143) ;;
        *) [ -z "$logged" ] || return 0 ;;
    esac
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$repo/build/tests/system/imagerace-probe.XXXXXX")
tree=$work/tree

# Report $* and exit 1, so that a failed setup or build is never taken for a
# stale image.
fail()
{
    echo "imagerace-probe: $*" >&2
    exit 1
}

# build LOG ARGS...: make ARGS with the whole output in LOG, in the
# background so that cleanup can stop it; a failure prints LOG and fails.
build()
{
    log=$1
    shift
    MAKEFLAGS='' MFLAGS='' make --no-print-directory "$@" > "$log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    if [ "$status" -ne 0 ]; then
        cat "$log" >&2
        fail "build failed: $log"
    fi
}

# image_make LOG [ARGS...]: make the private tree's test image, taking every
# component but tests as up to date, since the manifest stages one probe.
image_make()
{
    build "$@" -C "$tree" -j32 -o preflight -o krffs -o rach2 \
        -o ish -o utils -o tools/etude/guest -o musl-programs \
        -o vendor/musl/.stamp -o build/krffs/mkfs.krffs PROGS=conloadprobe DEMO_CREDS= \
        build/images/test.krffs
}

mkdir -p "$tree/tests/system" "$tree/krffs/manifests" "$tree/tools/inspect" \
    "$tree/tools/etude" "$tree/utils"
cp "$makefile" "$tree/Makefile"
cp -R mk "$tree/"
cp tools/etude/sources.mk "$tree/tools/etude/"
cp tests/Makefile tests/conloadprobe.c tests/probe.h tests/registry "$tree/tests/"
cp utils/pingcall.h "$tree/utils/"
cp tests/system/suites.mk "$tree/tests/system/"
cp krffs/*.[ch] krffs/Makefile krffs/mkimage.sh krffs/manifest_deps.sh krffs/programs.sh "$tree/krffs/"
cp krffs/manifests/*.manifest "$tree/krffs/manifests/"
cp tools/inspect/setidcheck.c "$tree/tools/inspect/"
printf '%s\n' 'exec-dir 0755 / build/tests' > "$tree/krffs/manifests/test.manifest"
printf '%s\n' 'manifest test' 'sentinel /conloadprobe 0755:0:0' 'init none' \
    > "$tree/tools/inspect/setid.allow"
echo "imagerace-probe: logs: $work"
logged=1
build "$work/tools.log" -C "$tree/krffs" -j4 -o libc-check \
    ../build/krffs/mkfs.krffs ../build/krffs/stageimage.krffs \
    ../build/krffs/readfile.krffs

# Build the image, edit the probe, and require one make to rebuild the probe
# and stage its bytes, and later makes to rebuild nothing. The edit is a
# second after the build, since GNU Make 3.81 compares whole seconds.
image_make "$work/initial.log"
probe=$tree/build/tests/conloadprobe
img=$tree/build/images/test.krffs
reader=$tree/build/krffs/readfile.krffs
"$reader" "$img" /conloadprobe "$work/initial.probe" || fail 'initial extraction failed'
cmp -s "$probe" "$work/initial.probe" || fail 'initial image differs from the probe'
sleep 1 || fail 'timestamp wait failed'
printf '\nconst char imagerace_words[] = "changed probe";\n' \
    >> "$tree/tests/conloadprobe.c" || fail 'probe edit failed'
image_make "$work/changed.log"
cmp -s "$probe" "$work/initial.probe" && fail 'the edited probe was not rebuilt'
cp "$probe" "$work/built.probe" || fail 'probe copy failed'
"$reader" "$img" /conloadprobe "$work/staged.probe" || fail 'changed extraction failed'
if ! cmp -s "$probe" "$work/staged.probe"; then
    cmp -s "$work/initial.probe" "$work/staged.probe" || fail 'unexpected staged bytes'
    fail 'fail: the image has the old probe after one make'
fi
image_make "$work/unchanged.log"
if grep -q '^krffs/mkimage.sh ' "$work/unchanged.log"; then
    fail 'an unchanged image was rebuilt'
fi
image_make "$work/dry.log" -n
if grep -q '^krffs/mkimage.sh ' "$work/dry.log"; then
    fail 'a dry run would rebuild an unchanged image'
fi
echo "imagerace-probe: pass: the edit was staged in one make; the unchanged image was kept"
