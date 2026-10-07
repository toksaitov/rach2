#!/bin/sh
# Exercise the root image rule with a compiled probe in a private tree, the
# i686 one whatever ARCH the run that starts it is for: neither the
# architecture, in the environment or in the make flags tests/run gives it,
# nor the path map an x86-64 make exports reaches the tree.
set -eu
unset ARCH RACH2_ARCH RACH2_PATH_MAP MAKEFLAGS MFLAGS

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
case ${1:-} in
    '') mutate=0 ;;
    --mutate) mutate=1 ;;
    --texts) mutate=2 ;;
    *) echo 'imagerace-probe: usage: [--mutate | --texts]' >&2; exit 2 ;;
esac

# drop_delegation MAKEFILE: print MAKEFILE without the rule that hands the
# musl programs to tests/Makefile, the fault; fail unless the rule is there
# once.
drop_delegation()
{
    awk '
        /^\$\(filter-out \$\(ARCH_BUILD\)\/tests\/musl\/%.*: tests$/ { found++; skip = 1; next }
        skip && /^\t/ { next }
        { skip = 0; print }
        END { if (found != 1) exit 1 }
    ' "$1"
}

if [ "$mutate" -eq 2 ]; then
    drop_delegation Makefile >/dev/null || {
        echo 'imagerace-probe: the delegation rule is not once in Makefile' >&2; exit 1;
    }
    echo 'imagerace-probe: texts: every fault is once in its source'
    exit 0
fi
mkdir -p build/tests/system
work=$(mktemp -d "$repo/build/tests/system/imagerace-probe.XXXXXX")
tree=$work/tree
child=

# Stop the build before restoring the private sources it reads.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    if [ -f "$work/Makefile.saved" ]; then
        cp "$work/Makefile.saved" "$tree/Makefile"
        cp "$work/conloadprobe.saved" "$tree/tests/conloadprobe.c"
        touch "$tree/Makefile" "$tree/tests/conloadprobe.c"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# A failed setup or build must not count as a detected stale image.
fail()
{
    echo "imagerace-probe: $*" >&2
    exit 1
}

# Keep each complete build log and let cleanup wait for its make process.
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

# Omit components absent from the one-probe manifest, not the tests sub-make.
image_make()
{
    build "$@" -C "$tree" -j32 -o preflight -o krffs -o rach2 \
        -o ish -o utils -o musl-programs -o vendor/musl/.stamp \
        -o build/krffs/mkfs.krffs PROGS=conloadprobe DEMO_CREDS= \
        build/images/test.krffs
}

mkdir -p "$tree/tests/system" "$tree/krffs/manifests" "$tree/tools/inspect" "$tree/utils"
cp Makefile "$tree/"
cp -R mk "$tree/"
cp tests/Makefile tests/conloadprobe.c tests/probe.h tests/registry "$tree/tests/"
cp utils/pingcall.h "$tree/utils/"
cp tests/system/suites.mk "$tree/tests/system/"
cp krffs/*.[ch] krffs/Makefile krffs/mkimage.sh krffs/manifest_deps.sh krffs/programs.sh "$tree/krffs/"
cp krffs/manifests/*.manifest "$tree/krffs/manifests/"
cp tools/inspect/setidcheck.c "$tree/tools/inspect/"
printf '%s\n' 'exec-dir 0755 / build/tests' > "$tree/krffs/manifests/test.manifest"
printf '%s\n' 'manifest test' 'sentinel /conloadprobe 0755:0:0' 'init none' \
    > "$tree/tools/inspect/setid.allow"
cp "$tree/Makefile" "$work/Makefile.saved"
cp "$tree/tests/conloadprobe.c" "$work/conloadprobe.saved"
echo "imagerace-probe: logs: $work"
build "$work/tools.log" -C "$tree/krffs" -j4 -o libc-check \
    ../build/krffs/mkfs.krffs ../build/krffs/stageimage.krffs \
    ../build/krffs/readfile.krffs

# Compare extracted bytes after one make; also require a real probe rebuild.
# Edits are a second apart because GNU Make 3.81 compares whole seconds.
check_phase()
{
    phase=$1
    cp "$work/conloadprobe.saved" "$tree/tests/conloadprobe.c" || fail 'source restoration failed'
    touch "$tree/tests/conloadprobe.c" || fail 'source timestamp update failed'
    rm -f "$tree/build/images/test.krffs" "$tree/build/tests/conloadprobe" || fail 'fixture reset failed'
    image_make "$work/$phase.initial.log"
    probe=$tree/build/tests/conloadprobe
    img=$tree/build/images/test.krffs
    reader=$tree/build/krffs/readfile.krffs
    "$reader" "$img" /conloadprobe "$work/$phase.initial.probe" || fail 'initial extraction failed'
    cmp -s "$probe" "$work/$phase.initial.probe" || fail 'initial image differs from the probe'
    sleep 1 || fail 'timestamp wait failed'
    printf '\nconst char imagerace_words[] = "changed probe";\n' \
        >> "$tree/tests/conloadprobe.c" || fail 'probe edit failed'
    image_make "$work/$phase.changed.log"
    cmp -s "$probe" "$work/$phase.initial.probe" && fail 'the edited probe was not rebuilt'
    cp "$probe" "$work/$phase.built.probe" || fail 'probe copy failed'
    "$reader" "$img" /conloadprobe "$work/$phase.staged.probe" || fail 'changed extraction failed'
    if ! cmp -s "$probe" "$work/$phase.staged.probe"; then
        echo "imagerace-probe: fail: $phase image has the old probe after one make" >&2
        cmp -s "$work/$phase.initial.probe" "$work/$phase.staged.probe" || fail 'unexpected staged bytes'
        return 1
    fi
    image_make "$work/$phase.unchanged.log"
    if grep -q '^krffs/mkimage.sh ' "$work/$phase.unchanged.log"; then
        fail 'an unchanged image was rebuilt'
    fi
    image_make "$work/$phase.dry.log" -n
    if grep -q '^krffs/mkimage.sh ' "$work/$phase.dry.log"; then
        fail 'a dry run would rebuild an unchanged image'
    fi
    echo "imagerace-probe: pass: $phase edit staged in one make; unchanged image kept"
}

check_phase fixed || exit 1
if [ "$mutate" -eq 1 ]; then
    drop_delegation "$work/Makefile.saved" > "$tree/Makefile" || fail 'delegation rule not found'
    touch "$tree/Makefile"
    if check_phase mutated; then
        fail 'the removed delegation rule was not detected'
    fi
    cp "$work/Makefile.saved" "$tree/Makefile"
    touch "$tree/Makefile"
    check_phase restored || exit 1
    cmp -s "$work/fixed.built.probe" "$work/restored.built.probe" || fail 'restored probe differs'
    echo 'imagerace-probe: pass: mutation detected and restored build passed'
fi
