#!/bin/sh
# Build the two poll orders from one private source tree for measurement.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
runner="$repo/build/tests/system/runutil"
checker="$repo/build/tools/inspect/elfcheck"
if [ ! -x "$runner" ] || [ ! -x "$checker" ]; then
    echo 'poll-kernels: build build/tests/system/runutil and build/tools/inspect/elfcheck first' >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-poll-kernels.XXXXXX")
cp -R rach2 mk "$work/"
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

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

build()
{
    order=$1
    echo "poll-kernels: building $order"
    # A source change within the object's timestamp unit must still compile.
    rm -f "$work/build/rach2/sys/io/poll.o" "$work/build/rach2/kernel"
    "$runner" watch poll-kernels 120 "$work/$order.jobs" \
        make -s -C "$work/rach2" -j4 "ELFCHECK=$checker" &
    child=$!
    rc=0
    wait "$child" || rc=$?
    child=
    [ "$rc" -eq 0 ] || exit "$rc"
    cp "$work/build/rach2/kernel" "$work/$order.kernel"
    cp "$work/build/rach2/.config" "$work/$order.config"
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
before = '''        /* Stay on every queue across the halt: a waker that runs while this
         * process is halted must be able to find it and make it READY. The
         * removes come after, and are no-ops for a record already taken off. */
        if (current_proc->state == PROC_BLOCKED) {
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
echo 'poll-kernels: result: both orders built; restored kernel matches'
