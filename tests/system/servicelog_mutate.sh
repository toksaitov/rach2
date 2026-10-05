#!/bin/sh
# Prove the service log suite with inits built from a private copy, on the
# kernel, programs and emulator of the architecture ARCH names.
#
# Usage: sh tests/system/servicelog_mutate.sh
#
# tests/system/suites/servicelog.sh boots a root whose init is init-shell and
# requires the log its services write to be emptied within the boot once it
# is past its cap, and removed at the next boot when a boot left it so. The
# init is built from a private copy of utils/ and mk/, never into the main
# tree's build/, five times: as it is, where the suite must pass; with the
# emptying within a boot taken out, where it must fail on the first boot;
# restored, where it must pass with the init the first phase had; with the
# removal at boot taken out, where it must fail on the second boot; and
# restored again. The kernel, BusyBox and `update` are the main tree's. The
# trap stops the suite's process group before it restores the private source.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
abuild=build$ARCH_SUFFIX
[ ! -d build/.run.lock ] || {
    echo 'servicelog-mutation: another suite owns this tree' >&2; exit 1;
}
mkdir -p build
work=$(mktemp -d "$repo/build/servicelog-proof.XXXXXX")
tree="$work/tree"
source="$tree/utils/init.c"
child=
changed=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

restore()
{
    cp "$work/init.saved" "$source"
    touch "$source"
    changed=
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Change the private init.c where the old text occurs exactly once.
mutate()
{
    changed=1
    python3 - "$source" "$1" "$2" <<'PY'
import pathlib
import sys

path, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(old) != 1:
    raise SystemExit("servicelog-mutation: pattern is not unique: " + old)
path.write_text(text.replace(old, new))
PY
    touch "$source"
}

run_phase()
{
    phase=$1
    expected=$2
    message=$3
    # The program goes first: a source restored within the second its program
    # was built reads as up to date to a make that compares whole seconds.
    rm -f "$tree/build$ARCH_SUFFIX/utils/sbin/init-shell"
    make -s -C "$tree/utils" "../build$ARCH_SUFFIX/utils/sbin/init-shell" "ARCH=$RACH2_ARCH" \
        >"$work/$phase.build.log" 2>&1 || {
        cat "$work/$phase.build.log" >&2; exit 1;
    }
    cp "$tree/build$ARCH_SUFFIX/utils/sbin/init-shell" "$work/$phase.init"
    cksum "$source" "$work/$phase.init" >"$work/$phase.artifacts"
    build/tests/system/runutil group sh tests/system/suites/servicelog.sh "$abuild"/rach2/kernel \
        build/tests/system/harness build/tools/image/mkmbr "$work/$phase.init" \
        "$abuild"/utils/update "$abuild"/vendor/busybox/busybox "$work/$phase" \
        >"$work/$phase.out" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$phase.out"
    if [ "$expected" = pass ]; then
        [ "$status" -eq 0 ] || exit 1
    else
        [ "$status" -ne 0 ] || {
            echo "servicelog-mutation: the fault passed: $phase" >&2; exit 1;
        }
    fi
    grep -Fq "$message" "$work/$phase.out" || {
        echo "servicelog-mutation: $phase did not end with: $message" >&2; exit 1;
    }
    echo "servicelog-mutation: result: $phase status $status"
}

echo "servicelog-mutation: logs: $work"
make -s rach2 utils check-busybox build/tests/system/harness build/tests/system/runutil \
    build/tools/image/mkmbr "ARCH=$RACH2_ARCH" >"$work/prepare.log" 2>&1 || {
    cat "$work/prepare.log" >&2; exit 1;
}
# The private tree has what the utilities' Makefile reads, utils/ and mk/,
# and the architecture's ported musl it links against, which it only reads,
# at the same place under the tree as in the main one.
musl_link=$tree${MUSL_PREFIX#"$repo"}
mkdir -p "$(dirname "$musl_link")"
cp -R utils mk "$tree/"
ln -s "$MUSL_PREFIX" "$musl_link"
cp "$source" "$work/init.saved"
passed='servicelog-test: PASS'
run_phase fixed pass "$passed"

mutate '    if (st.st_size < SERVICE_LOG_CAP) {' '    if (st.st_size >= 0) {'
run_phase bound-removed fail 'servicelog-test: FAIL: the log was not emptied within the boot'
restore
run_phase bound-restored pass "$passed"
cmp "$work/fixed.init" "$work/bound-restored.init"

mutate '    if (stat(SERVICE_LOG, &st) == 0 && st.st_size >= SERVICE_LOG_CAP) {' \
    '    if (0 && stat(SERVICE_LOG, &st) == 0 && st.st_size >= SERVICE_LOG_CAP) {'
run_phase removal-removed fail 'servicelog-test: FAIL: the log the boot before left full was not removed'
restore
run_phase removal-restored pass "$passed"
cmp "$work/fixed.init" "$work/removal-restored.init"
echo 'servicelog-mutation: result: pass: both faults failed their boots; both restorations passed and matched'
