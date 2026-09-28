#!/bin/sh
# Prove the barriers of the two FAT drivers one at a time, entry 32 of
# milestone 50. Every call to `sync_disk` and `block_flush` in
# rach2/sys/fs/exfat.c and rach2/sys/fs/fat32.c, the flush inside
# `sync_disk` itself excepted, is a barrier site. Each site is dropped in
# turn from a private copy of its driver, the call replaced by the success
# it would have returned, and the driver's host test is run over the copy
# in its `--barriers` mode: the interruption matrix and the durability
# rows, with the free-durability row for exFAT. A site is proved when its
# run stops at a failed row that judges the medium, a state a crash leaves
# or the medium after a sync, with the test's failure status 1, and
# unproved when its run passes. A run that ends any other way, killed by a
# signal or by its alarm, stopped by a sanitizer report or by a fixture
# that did not build, or failed at a row of the rows' own setup, proves
# nothing about the site, and the proof stops.
#
# After each drop both sources are restored and compared with the copies
# the unmodified run passed on. The restored driver is compiled and run
# after its last site, and must pass. Every restore writes the same bytes
# and every compile starts from the sources, so a run after each site
# would compile the same program fifteen times for each driver.
#
# One site is unproved by construction: fat_rmdir's flush in fat32.c comes
# right before `free_chain`, whose first step is a flush of its own, with
# no write between the two, so the device sees the same order of writes
# with either dropped and no row can tell them apart. The proof passes
# when every other site is proved and that one alone is not.
#
# Usage: fat_barriers_mutate.sh [--check]
# With --check it compiles and runs the two tests' barrier rows over the
# unmodified drivers alone.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
[ "$#" -eq 0 ] || [ "$*" = --check ] || {
    echo 'fat-barriers-proof: usage: fat_barriers_mutate.sh [--check]' >&2
    exit 2
}
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/fat-barriers-proof.XXXXXX")
# The tests' fixtures, in a directory of this run's that the trap removes: a
# test given none makes its own and keeps it when it fails, as a dropped
# barrier makes it do.
fixtures=$(mktemp -d /tmp/rach2-fix.XXXXXX)
mkdir -p "$work/rach2" "$work/tests/host"
cp -R rach2/sys "$work/rach2/sys"
cp tests/host/exfat_hosttest.c tests/host/fat32_hosttest.c "$work/tests/host/"
for driver in exfat fat32; do
    cp "$work/rach2/sys/fs/$driver.c" "$work/$driver.saved"
done
child=

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

# The loop variable is its own: a shell function shares its caller's
# variables, and the site loop below keeps its driver in `driver`.
restore()
{
    for each in exfat fat32; do
        cp "$work/$each.saved" "$work/rach2/sys/fs/$each.c"
        touch "$work/rach2/sys/fs/$each.c"
    done
}

cleanup()
{
    stop_child
    restore
    rm -rf "$fixtures"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The sites of a driver, one line each: the index, the line, the enclosing
# function and the call; or, with `drop INDEX`, the driver rewritten with
# that site's call replaced by 0.
cat >"$work/sites.py" <<'PY'
from pathlib import Path
import re
import sys


def sites(text):
    lines = text.split('\n')
    found = []
    for m in re.finditer(r'\b(sync_disk|block_flush)\(', text):
        line_no = text.count('\n', 0, m.start()) + 1
        if not lines[line_no - 1][:1].isspace():
            continue  # the definition of sync_disk
        function = None
        for i in range(line_no - 2, -1, -1):
            header = re.match(r'^[A-Za-z_][\w \*]*?\b(\w+)\(', lines[i])
            if header and not lines[i].rstrip().endswith(';'):
                function = header.group(1)
                break
        if function is None:
            raise SystemExit('fat-barriers-proof: a site outside a function at line %d' % line_no)
        if function == 'sync_disk':
            continue  # its flush is part of every site that calls it
        depth, j = 0, m.end() - 1
        while True:
            if text[j] == '(':
                depth += 1
            elif text[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        found.append((m.start(), j + 1, line_no, function, text[m.start():j + 1]))
    return found


path = Path(sys.argv[1])
text = path.read_text()
found = sites(text)
if sys.argv[2] == 'list':
    for index, (_, _, line_no, function, call) in enumerate(found):
        print('%d\t%d\t%s\t%s' % (index, line_no, function, call))
else:
    start, end = found[int(sys.argv[3])][:2]
    path.write_text(text[:start] + '0' + text[end:])
PY

# compile DRIVER: the driver's host test over the private tree, into
# $work/DRIVER.bin; the test's own dependency lines, as the host suite
# reads them, with the kernel's sources taken from the private tree.
compile()
{
    hosttest="$work/tests/host/$1_hosttest.c"
    deps=$(grep -oE 'host-deps:[^*]*' "$hosttest" | sed 's/host-deps://' | tr '\n' ' ' |
        sed "s# rach2/# $work/rach2/#g")
    # shellcheck disable=SC2086
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -DFIXDIR="\"$fixtures\"" \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
        -Wno-empty-body -ffreestanding \
        -I "$work/rach2/sys" -I "$work/rach2/sys/lib" -I "$work/rach2/sys/proc" \
        -I utils -I krffs -I tests/host/compat \
        -o "$work/$1.bin" "$hosttest" $deps </dev/null >"$work/$1.compile.log" 2>&1; then
        cat "$work/$1.compile.log" >&2
        exit 1
    fi
}

# run DRIVER LOG: the barrier rows; `status` is the exit status.
run()
{
    cksum "$work/rach2/sys/fs/$1.c" "$work/$1.bin" >"$2.cksum"
    perl -e 'setpgrp(0, 0) or die "fat-barriers-proof: setpgrp: $!\n";
             alarm 900; exec @ARGV or die "fat-barriers-proof: exec: $!\n"' \
        "$work/$1.bin" --barriers </dev/null >"$2" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
}

# judge DRIVER LOG: how the run that wrote LOG ended, in `outcome`: `pass`
# when the rows passed, `fail` when it stopped with status 1 at a failed
# row that judges the medium, and empty for any other end. With
# `--barriers` a failed row ends the run, so it is the log's last line,
# which is left in `last`. The patterns are the names those rows have in
# the two tests: a crash state mounted, read and walked, the medium a
# failed barrier leaves, and the medium after a sync or an unlink.
judge()
{
    outcome=
    last=$(tail -n 1 "$2")
    if [ "$status" -eq 0 ] && grep -q "^$1_hosttest: OK" "$2"; then
        outcome=pass
    elif [ "$status" -eq 1 ]; then
        if [ "$1" = exfat ]; then
            fail='exfat_hosttest: FAIL: '
        else
            fail='FAIL: '
        fi
        case $last in
            "$fail"*' of the pending writes kept, '* | \
                "$fail"*' failed leaves a consistent volume' | \
                "$fail"'the medium after the sync mounts' | \
                "$fail"'and the file is on the medium with its bytes, nothing pending kept' | \
                "$fail"'the two clusters an unlink frees are free on the medium when it returns')
                outcome=fail
                ;;
        esac
    fi
}

# must_pass DRIVER STATE: compile the driver's test over the private tree,
# its source STATE, `unmodified` or `restored`, and require its barrier
# rows to pass.
must_pass()
{
    compile "$1"
    run "$1" "$work/$1.$2.log"
    judge "$1" "$work/$1.$2.log"
    if [ "$outcome" != pass ]; then
        cat "$work/$1.$2.log"
        echo "fat-barriers-proof: fail: the $2 $1 driver fails its barrier rows" >&2
        exit 1
    fi
    echo "fat-barriers-proof: result: the $2 $1 driver passes its barrier rows"
}

echo "fat-barriers-proof: logs: $work"
for driver in exfat fat32; do
    must_pass "$driver" unmodified
done
[ "$#" -eq 0 ] || exit 0

# The sites each driver has; a count that moved means a site was added or
# removed, and the proof and its expectation are to be read again.
python3 "$work/sites.py" "$work/exfat.saved" list >"$work/exfat.sites"
python3 "$work/sites.py" "$work/fat32.saved" list >"$work/fat32.sites"
for pair in exfat:15 fat32:15; do
    driver=${pair%%:*}
    want=${pair#*:}
    have=$(wc -l <"$work/$driver.sites" | tr -d ' ')
    if [ "$have" -ne "$want" ]; then
        cat "$work/$driver.sites"
        echo "fat-barriers-proof: fail: $driver.c has $have barrier sites, not $want" >&2
        exit 1
    fi
done

unproved=
for driver in exfat fat32; do
    while IFS="$(printf '\t')" read -r index line function call; do
        site="$driver.c:$line $function $call"
        restore
        python3 "$work/sites.py" "$work/rach2/sys/fs/$driver.c" drop "$index"
        touch "$work/rach2/sys/fs/$driver.c"
        compile "$driver"
        run "$driver" "$work/$driver.site$index.log"
        restore
        cmp "$work/exfat.saved" "$work/rach2/sys/fs/exfat.c"
        cmp "$work/fat32.saved" "$work/rach2/sys/fs/fat32.c"
        judge "$driver" "$work/$driver.site$index.log"
        case $outcome in
            fail) echo "fat-barriers-proof: proved: $site: $last" ;;
            pass)
                echo "fat-barriers-proof: unproved: $site"
                unproved="$unproved$driver:$function:$call;"
                ;;
            *)
                echo "fat-barriers-proof: fail: $site: status $status, not a failed row that judges the medium: $last" >&2
                exit 1
                ;;
        esac
    done <"$work/$driver.sites"
    must_pass "$driver" restored
done
if [ "$unproved" != 'fat32:fat_rmdir:block_flush(fm->dev);' ]; then
    echo "fat-barriers-proof: fail: unproved sites: ${unproved:-none}; expected fat32.c's fat_rmdir flush alone" >&2
    exit 1
fi
echo 'fat-barriers-proof: result: pass: every barrier site but the flush in fat_rmdir, which free_chain repeats, fails a row that judges the medium when dropped, and each restored driver passes'
