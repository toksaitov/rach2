#!/usr/bin/env sh

#
# build_kvm.sh: stage the KVM layer (Readme.md) for the x86-64 GNU root.
#
# Usage: build_kvm.sh [STEP...]
#
# The steps are qemu, unit, selftests and check; with no STEP all four run
# from nothing, and naming steps runs only those over what an earlier run
# left. `qemu` stages the Debian closure of qemu.list as the layer `kvm` of
# ../debian/fetch_debs.sh, in debian/kvm of the layer's build tree, with its
# file lists and closure under work/debian/kvm. `unit` builds kvm-unit-tests
# and `selftests` Linux's KVM selftests, from the sources sources.txt pins,
# with the cross toolchain `make gnu-toolchain` built, into kvm/opt of the
# build tree, the staging root that install_kvm.sh installs. `check` boots
# kvm-unit-tests' sieve under TCG with the staged QEMU and runs each selftest
# where there is no /dev/kvm, where each must report its skip; both run only
# on an x86-64 host. Each step writes its output to a log in log/ of the
# build tree. Under make the sub-makes share the caller's jobs; run alone,
# the script uses GNU_JOBS processors, or every processor when GNU_JOBS is
# not set.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
# shellcheck disable=SC2034 # arch.sh reads it
REPO="$(cd "$GNU/../.." && pwd)"
. "$GNU/arch.sh"
. "$GNU/debian/fetch_asset.sh"
OUT="$GNU_OUT"
SRC="$OUT/src/kvm"
WORK="$OUT/work/kvm"
LOG="$OUT/log"
CROSS="$OUT/cross"
ROOT="$OUT/kvm"
UNIT="$ROOT/opt/kvm-unit-tests"
SELF="$ROOT/opt/kvm-selftests"
STAGE="$OUT/debian/kvm"
TARGET="$GNU_TARGET"
PATH="$GNU_PATH$CROSS/bin:$PATH"
export PATH

case " $MAKEFLAGS " in
    *jobserver*) JOBS= ;;
    *) JOBS="-j${GNU_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}" ;;
esac

if [ "$RACH2_ARCH" != x86_64 ]; then
    echo "gnu: kvm: the KVM layer is the x86-64 root's; run with RACH2_ARCH=x86_64" >&2
    exit 1
fi
if [ ! -x "$CROSS/bin/$TARGET-gcc" ]; then
    echo "gnu: kvm: no cross compiler; run make ARCH=x86_64 gnu-toolchain first" >&2
    exit 1
fi
for tool in cc make tar xz gzip patch unshare; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "gnu: kvm: $tool not found; the GNU layer builds on a Linux host with the tools ../Readme.md lists" >&2
        exit 1
    fi
done

# extract NAME [EXCLUDE...]: unpack the checked source NAME afresh, less the
# top-level directories EXCLUDE names, and print its tree.
extract()
{
    name=$1
    shift
    tarball="$(asset "$name" "$DIR/sources.txt")"
    rm -rf "${SRC:?}/$name"
    mkdir -p "$SRC/$name"
    top="$(tar -tf "$tarball" | sed -n '1s|/.*||p')"
    excludes=
    for d in "$@"; do
        excludes="$excludes --exclude=$top/$d"
    done
    # shellcheck disable=SC2086 # the names have no spaces
    tar -xf "$tarball" -C "$SRC/$name" $excludes
    echo "$SRC/$name/$top"
}

# pin NAME: the version field of NAME's line in sources.txt.
pin()
{
    awk -v n="$1" '$1 == n { print $2; exit }' "$DIR/sources.txt"
}

step_qemu()
{
    GNU_DEBIAN_LISTS=kvm/qemu.list "$GNU/debian/fetch_debs.sh" kvm
}

# kvm-unit-tests' Makefile writes build-head, which run_tests.sh prints, from
# `git rev-parse HEAD` in the source tree, which would name the commit of a
# repository the build tree is in; the ceiling keeps git out, and the file
# gets the pinned commit, as a checkout of it would have. The staged tree is
# what run_tests.sh reads, with config.mak naming its own directory and
# x86_64 as its host, which configure takes from the build host's uname and
# the runner compares with the tests' architecture before it uses /dev/kvm;
# stacks are printed raw, since pretty_print_stacks.py needs the cross
# toolchain's addr2line and the ELF files, which are not staged.
step_unit()
{
    s="$(extract kvm-unit-tests)"
    cd "$s"
    ./configure --arch=x86_64 --cross-prefix="$TARGET-"
    GIT_CEILING_DIRECTORIES="$SRC/kvm-unit-tests" make ${JOBS:+"$JOBS"}
    commit="$(pin kvm-unit-tests)"
    printf '%.8s\n' "$commit" > build-head
    rm -rf "$UNIT"
    mkdir -p "$UNIT/scripts" "$UNIT/x86"
    cp COPYRIGHT LICENSE README.md errata.txt run_tests.sh build-head "$UNIT/"
    cp scripts/arch-run.bash scripts/common.bash scripts/runtime.bash \
        scripts/vmm.bash "$UNIT/scripts/"
    cp x86/run x86/unittests.cfg x86/*.flat "$UNIT/x86/"
    sed -e "s|^HOST=.*|HOST=x86_64|" \
        -e "s|^SRCDIR=.*|SRCDIR=/opt/kvm-unit-tests|" \
        -e "s|^ERRATATXT=.*|ERRATATXT=/opt/kvm-unit-tests/errata.txt|" \
        -e "s|^PRETTY_PRINT_STACKS=.*|PRETTY_PRINT_STACKS=no|" config.mak > "$UNIT/config.mak"
    {
        echo "kvm-unit-tests $commit"
        echo "configure: --arch=x86_64 --cross-prefix=$TARGET-"
        "$TARGET-gcc" --version | sed -n 1p
        echo "tests: $(find x86 -name '*.flat' | wc -l | tr -d ' ')"
    } > "$UNIT/build-info"
    cat "$UNIT/build-info"
}

# The selftests need the kernel's exported headers, which `make headers`
# installs in the tree's usr/include with the build host's compiler; the
# directories the build reads nothing from are not unpacked. Each test is
# linked static with the cross toolchain's glibc, as 80-2 linked it with
# Debian's, and its debugging information is stripped. The staged tree has
# the tests where the selftests' tree has them, x86_64/ for the x86 ones,
# with x86_64/nx_huge_pages_test.sh, the one test that is a script, and
# list, the tests the Makefile builds for x86, one a line. The tree takes
# max-guest-memory-one-cpu.patch first, which build-info names and the
# staged tree keeps beside the licenses.
step_selftests()
{
    s="$(extract linux-kvm-selftests drivers Documentation sound fs net)"
    patch -d "$s" -p1 -F 0 --forward < "$DIR/max-guest-memory-one-cpu.patch"
    make -C "$s" ARCH=x86 headers
    rm -rf "$WORK/selftests"
    mkdir -p "$WORK/selftests"
    make -C "$s/tools/testing/selftests/kvm" ${JOBS:+"$JOBS"} ARCH=x86 \
        CROSS_COMPILE="$TARGET-" EXTRA_CFLAGS=-static OUTPUT="$WORK/selftests"
    make -s -C "$s/tools/testing/selftests/kvm" ARCH=x86 OUTPUT="$WORK/selftests" \
        -f Makefile -f - list > "$WORK/list" <<'EOF'
list:
	@for t in $(TEST_GEN_PROGS) $(TEST_GEN_PROGS_EXTENDED); do echo $${t#$(OUTPUT)/}; done
EOF
    rm -rf "$SELF"
    mkdir -p "$SELF/x86_64"
    while read -r t; do
        "$TARGET-strip" --strip-debug -o "$SELF/$t" "$WORK/selftests/$t"
    done < "$WORK/list"
    cp "$s/tools/testing/selftests/kvm/x86_64/nx_huge_pages_test.sh" "$SELF/x86_64/"
    cp "$WORK/list" "$SELF/list"
    cp "$s/COPYING" "$SELF/COPYING"
    cp "$s/LICENSES/preferred/GPL-2.0" "$SELF/GPL-2.0"
    cp "$DIR/max-guest-memory-one-cpu.patch" "$SELF/"
    {
        echo "linux $(pin linux-kvm-selftests) tools/testing/selftests/kvm"
        echo "patch: max-guest-memory-one-cpu.patch"
        echo "make: ARCH=x86 CROSS_COMPILE=$TARGET- EXTRA_CFLAGS=-static"
        "$TARGET-gcc" --version | sed -n 1p
        echo "tests: $(wc -l < "$WORK/list" | tr -d ' ')"
    } > "$SELF/build-info"
    cat "$SELF/build-info"
}

# The sieve boots as kvm-unit-tests' x86/run boots a test, with the staged
# QEMU run by the staged loader on the staged libraries, which finds its
# firmware where the root has it; the test passes when QEMU exits with 1,
# isa-debug-exit's code for a test's status 0. Each selftest runs in a root
# of its own that has no /dev, under a namespace of the build's account, and
# must exit with 4, the kselftest code for a skip.
step_check()
{
    unit=$(find "$UNIT/x86" -name '*.flat' | wc -l | tr -d ' ')
    tests=$(wc -l < "$SELF/list" | tr -d ' ')
    while read -r t; do
        [ -x "$SELF/$t" ] || { echo "gnu: kvm: check: no selftest $t" >&2; return 1; }
    done < "$SELF/list"
    if [ "$(uname -m)" != x86_64 ]; then
        echo "gnu: kvm: check: $unit kvm-unit-tests and $tests selftests staged; nothing run: this $(uname -m) host runs x86-64 programs through QEMU's emulator"
        return 0
    fi
    lib="$STAGE/usr/lib/x86_64-linux-gnu:$STAGE/usr/lib"
    set +e
    "$STAGE/usr/lib64/ld-linux-x86-64.so.2" --library-path "$lib" \
        "$STAGE/usr/bin/qemu-system-x86_64" -L "$STAGE/usr/share/qemu" \
        -L "$STAGE/usr/share/seabios" --no-reboot -nodefaults \
        -global kvm-pit.lost_tick_policy=discard -device pc-testdev \
        -device isa-debug-exit,iobase=0xf4,iosize=0x4 -display none \
        -serial stdio -device pci-testdev -machine accel=tcg \
        -kernel "$UNIT/x86/sieve.flat" -smp 1 < /dev/null > "$WORK/sieve.out" 2>&1
    status=$?
    set -e
    cat "$WORK/sieve.out"
    if [ "$status" -ne 1 ]; then
        echo "gnu: kvm: check: sieve: QEMU exited with $status, not 1" >&2
        return 1
    fi
    r="$WORK/check"
    rm -rf "$r"
    mkdir -p "$r/proc" "$r/tmp" "$r/opt"
    cp -a "$SELF" "$r/opt/"
    skipped=0
    while read -r t; do
        set +e
        HOME=/tmp unshare --map-root-user --mount --pid --fork --mount-proc=/proc \
            --root="$r" "/opt/kvm-selftests/$t" < /dev/null > "$WORK/test.out" 2>&1
        status=$?
        set -e
        echo "$t: $status: $(tail -n 1 "$WORK/test.out")"
        if [ "$status" -ne 4 ]; then
            cat "$WORK/test.out"
            echo "gnu: kvm: check: $t exited with $status, not 4, the skip" >&2
            return 1
        fi
        skipped=$((skipped + 1))
    done < "$SELF/list"
    rm -rf "$r" "$WORK/test.out"
    echo "gnu: kvm: check: result: sieve passed under TCG with the staged QEMU; $skipped of $tests selftests skipped without /dev/kvm"
}

if [ $# -eq 0 ]; then
    rm -rf "$SRC" "$WORK" "$ROOT"
    set -- qemu unit selftests check
fi
mkdir -p "$LOG" "$WORK"
for step in "$@"; do
    case "$step" in
        qemu|unit|selftests|check) ;;
        *) echo "gnu: kvm: no step named $step" >&2; exit 2 ;;
    esac
    echo "gnu: kvm: $step"
    log="$LOG/kvm-$step.log"
    t0=$(date +%s)
    set +e
    ( set -e; "step_$step" ) > "$log" 2>&1
    status=$?
    set -e
    if [ $status -ne 0 ]; then
        tail -n 40 "$log" >&2
        echo "gnu: kvm: $step: failed; the log is $log" >&2
        exit 1
    fi
    case "$step" in
        unit|selftests) tail -n 4 "$log" ;;
        check) tail -n 1 "$log" ;;
    esac
    echo "gnu: kvm: $step: done in $(($(date +%s) - t0)) s"
done
