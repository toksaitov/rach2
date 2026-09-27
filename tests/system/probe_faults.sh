#!/bin/sh
# Prove rows of the default suite against the faults they catch. Each fault
# in tests/system/probe_faults.txt names a probe, a file, a text of that file
# with the text that replaces it, and the words the probe prints only when
# the row fails. The fault is put alone into this tree, `make run-default`
# rebuilds what it changed and boots the suite, and the fault is caught when
# the suite fails, the runner does not pass the probe, and the boot's log has
# the probe's words. A fault that hangs the probe is caught the same way: its
# words come before the hang, and the runner's deadline ends the boot.
#
# The file is restored and touched after each fault, and the trap restores it
# and rebuilds if the run is stopped, after the suite's processes are
# stopped. The kernel and the programs rebuilt at the end must be the first
# ones, byte for byte, so the full set run on this tree afterwards is what
# shows each restored row passing; this script does not run the rows on the
# unmodified tree. It refuses to start while a suite holds the run lock.
#
# NAME arguments run those faults alone. -s K/N runs the faults whose place
# in the list, counted from 0, is K modulo N, so that N copies of the tree can
# share the list. Every fault runs even when an earlier one was not caught,
# and the exit status is 1 when any was not.
#
# Usage: sh tests/system/probe_faults.sh [-s K/N] [NAME...]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
list=tests/system/probe_faults.txt
shard=0/1
if [ "${1:-}" = -s ] && [ "$#" -ge 2 ]; then
    shard=$2
    shift 2
fi
case ${1:-} in
    -*) echo 'probe-faults: usage: probe_faults.sh [-s K/N] [NAME...]' >&2; exit 2 ;;
esac
[ ! -d build/.run.lock ] || {
    echo 'probe-faults: another suite owns this tree' >&2; exit 1;
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/probe-faults.XXXXXX")

# `faults select SHARD [NAME...]` prints the chosen faults, one a line, as
# NAME, PROBE, FILE and WORDS separated by tabs; `faults apply NAME` puts that
# fault into its file. A list whose text does not occur exactly once in its
# file, or an unknown name, stops the run before any fault is applied.
faults()
{
    python3 - "$list" "$@" <<'PY'
import sys
from pathlib import Path

def parse(path):
    faults, cur, part = [], None, None
    for n, line in enumerate(Path(path).read_text().split('\n'), 1):
        if cur is None:
            if line.startswith('fault '):
                cur = {'name': line[6:], 'old': [], 'new': [], 'line': n}
            elif line and not line.startswith('#'):
                sys.exit('probe-faults: %s:%d: not in a fault: %s' % (path, n, line))
        elif part is None and line == 'old':
            part = 'old'
        elif part is None:
            key, _, value = line.partition(' ')
            if key not in ('probe', 'file', 'expect') or not value:
                sys.exit('probe-faults: %s:%d: bad field: %s' % (path, n, line))
            cur[key] = value
        elif part == 'old' and line == 'new':
            part = 'new'
        elif part == 'new' and line == 'end':
            for key in ('probe', 'file', 'expect'):
                if key not in cur:
                    sys.exit('probe-faults: %s: %s has no %s' % (path, cur['name'], key))
            faults.append(cur)
            cur, part = None, None
        else:
            cur[part].append(line)
    if cur is not None:
        sys.exit('probe-faults: %s: %s has no end' % (path, cur['name']))
    return faults

def text(lines):
    return '\n'.join(lines) + '\n'

path, verb = sys.argv[1], sys.argv[2]
faults = parse(path)
names = [f['name'] for f in faults]
if len(set(names)) != len(names):
    sys.exit('probe-faults: %s: a name is used twice' % path)
for f in faults:
    if Path(f['file']).read_text().count(text(f['old'])) != 1:
        sys.exit('probe-faults: %s: the text is not once in %s' % (f['name'], f['file']))
if verb == 'select':
    k, _, n = sys.argv[3].partition('/')
    if not (k.isdigit() and n.isdigit() and int(k) < int(n)):
        sys.exit('probe-faults: bad shard: %s' % sys.argv[3])
    wanted = sys.argv[4:]
    for name in wanted:
        if name not in names:
            sys.exit('probe-faults: no fault %s' % name)
    for i, f in enumerate(faults):
        if (f['name'] in wanted) if wanted else (i % int(n) == int(k)):
            print('\t'.join((f['name'], f['probe'], f['file'], f['expect'])))
elif verb == 'apply':
    f = faults[names.index(sys.argv[3])]
    p = Path(f['file'])
    p.write_text(p.read_text().replace(text(f['old']), text(f['new'])))
PY
}

faults select "$shard" "$@" >"$work/selected"
[ -s "$work/selected" ] || { echo 'probe-faults: no fault selected' >&2; exit 2; }
tab=$(printf '\t')
while IFS=$tab read -r _ _ file _; do
    if [ ! -f "$work/saved/$file" ]; then
        mkdir -p "$work/saved/$(dirname "$file")"
        cp "$file" "$work/saved/$file"
    fi
done <"$work/selected"
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"
child=
changed=
missed=

stop_child()
{
    if [ -n "$child" ]; then
        "$work/runutil" killgroup "$child" 15 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

restore()
{
    if [ -n "$changed" ]; then
        cp "$work/saved/$changed" "$changed"
        touch "$changed"
        changed=
    fi
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        make -s rach2 test-image >"$work/cleanup.build.log" 2>&1 </dev/null || :
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The kernel and the programs of build/tests, build/tests/musl and
# build/utils, as checksums in a stable order.
products()
{
    find build/rach2/kernel build/tests build/tests/musl build/utils -maxdepth 1 -type f \
        ! -name '*.d' ! -name '*.log' -exec cksum {} + | sort -k 3
}

# One boot of the default suite with fault $1 compiled in. Its output is kept
# in $work/NAME.log and printed, and the boot's log in $work/NAME.guest.log.
probe_fault()
{
    name=$1 probe=$2 file=$3 words=$4
    changed=$file
    faults apply "$name"
    touch "$file"
    rm -f build/images/default-test.log
    "$work/runutil" group make run-default >"$work/$name.log" 2>&1 </dev/null &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$work/$name.log"
    [ ! -f build/images/default-test.log ] || cp build/images/default-test.log "$work/$name.guest.log"
    restore
    if ! grep -q '^runtests: [0-9]* of [0-9]* probes' "$work/$name.log"; then
        echo "probe-faults: $name: the suite did not run; see $work/$name.log" >&2
        missed="$missed $name"
    elif [ "$status" -ne 0 ] && grep -Fqs -- "$words" "$work/$name.guest.log" &&
        ! grep -Eq "^  (musl/)?$probe +.* PASS\$" "$work/$name.log"; then
        echo "probe-faults: result: $name failed the row of $probe, status $status"
    else
        echo "probe-faults: $name did not fail the row of $probe" >&2
        missed="$missed $name"
    fi
}

echo "probe-faults: logs: $work"
make -s rach2 test-image >"$work/first.build.log" 2>&1 </dev/null ||
    { cat "$work/first.build.log"; exit 1; }
products >"$work/first.sums"
while IFS=$tab read -r name probe file words; do
    probe_fault "$name" "$probe" "$file" "$words"
done <"$work/selected"

make -s rach2 test-image >"$work/restored.build.log" 2>&1 </dev/null ||
    { cat "$work/restored.build.log"; exit 1; }
products >"$work/restored.sums"
diff "$work/first.sums" "$work/restored.sums" ||
    { echo 'probe-faults: the restored products differ from the first' >&2; exit 1; }
while IFS=$tab read -r _ _ file _; do
    cmp "$file" "$work/saved/$file"
done <"$work/selected"
if [ -n "$missed" ]; then
    echo "probe-faults: result: fail: not caught:$missed" >&2
    exit 1
fi
echo 'probe-faults: result: pass: each fault failed its row; the restored products equal the first'
