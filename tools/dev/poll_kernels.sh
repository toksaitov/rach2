#!/bin/sh
# Build the two poll orders from one private source tree for measurement.
# Each kernel's path is printed as RACH2_MEASURE_KERNEL, the name
# machine_identity.sh takes, so that a measurement driver checks that the
# machine runs that artifact before it times anything. The kernels are built
# for the architecture ARCH names.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
out=build$ARCH_SUFFIX/rach2
runner="$repo/build/tests/system/runutil"
checker="$repo/build/tools/inspect/elfcheck"
if [ ! -x "$runner" ] || [ ! -x "$checker" ]; then
    echo 'poll-kernels: build build/tests/system/runutil and build/tools/inspect/elfcheck first' >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-poll-kernels.XXXXXX")
cp -R rach2 mk "$work/"
# rach2/Makefile's kernel rule runs `make build/tools/inspect/elfcheck` in the
# directory above rach2/ before it links the kernel and checks it with
# $(ELFCHECK). The private tree's top Makefile has an empty rule for that
# target, and ELFCHECK is set to the checker built in the main tree.
printf 'build/tools/inspect/elfcheck: ;\n' >"$work/Makefile"
poll="$work/rach2/sys/io/poll.c"
cp "$poll" "$work/poll.saved"
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/poll.saved" "$poll"
    touch "$poll"
}

# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
status=0
trap 'status=$?; cleanup; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

build()
{
    order=$1
    echo "poll-kernels: building $order"
    # A source change within the object's timestamp unit must still compile.
    rm -f "$work/$out/sys/io/poll.o" "$work/$out/kernel"
    "$runner" watch poll-kernels 120 "$work/$order.jobs" \
        make -s -C "$work/rach2" -j4 "ELFCHECK=$checker" "ARCH=$RACH2_ARCH" &
    child=$!
    rc=0
    wait "$child" || rc=$?
    child=
    [ "$rc" -eq 0 ] || exit "$rc"
    cp "$work/$out/kernel" "$work/$order.kernel"
    cp "$work/$out/.config" "$work/$order.config"
    shasum -a 256 "$work/$order.kernel"
}

echo "poll-kernels: artifacts: $work"
git rev-parse HEAD > "$work/base.commit"
git diff --binary > "$work/working.diff"
build poll-after-halt
python3 - "$poll" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
before = '''        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off();
        }
        for (i = 0; i < nfds; i++) {
            wq_poll_remove(wqs[i], &recs[i]);
        }
'''
after = '''        for (i = 0; i < nfds; i++) {
            wq_poll_remove(wqs[i], &recs[i]);
        }
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off();
        }
'''
if source.count(before) != 1:
    sys.exit("poll-kernels: the poll block no longer matches")
path.write_text(source.replace(before, after))
PY
touch "$poll"
cp "$poll" "$work/poll.before-halt.c"
diff -u "$work/poll.saved" "$poll" > "$work/poll-order.diff" || [ "$?" -eq 1 ]
cat "$work/poll-order.diff"
build poll-before-halt
if cmp -s "$work/poll-after-halt.kernel" "$work/poll-before-halt.kernel"; then
    echo 'poll-kernels: the changed poll order produced the same kernel' >&2
    exit 1
fi
cleanup
build poll-restored
cmp "$work/poll-after-halt.kernel" "$work/poll-restored.kernel"
cmp "$work/poll-after-halt.config" "$work/poll-before-halt.config"
cmp "$work/poll-after-halt.config" "$work/poll-restored.config"
for order in poll-after-halt poll-before-halt; do
    echo "poll-kernels: measure: RACH2_MEASURE_KERNEL=$work/$order.kernel"
done
echo 'poll-kernels: result: both orders built; restored kernel matches'
finished=1
