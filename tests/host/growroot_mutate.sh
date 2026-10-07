#!/bin/sh
# Exercise table validation and phase measurements in private builds.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# apply_unchecked_read ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_unchecked_read()
{
    python3 - "$1" <<'PY'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = pathlib.Path(sys.argv[1])
source = path.read_text()
before = '''    if (read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
        close(fd);
        return fail(dev, "the first sector did not read; nothing written");
    }
    {
'''
after = '''    if (read(fd, sector, sizeof(sector)) == (ssize_t)sizeof(sector)) {
'''
write = '''    e = sector + MBR_ENTRIES + 16 * (entry - 1);
'''
reread = '''    if (lseek(fd, 0, SEEK_SET) != 0 ||
        read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector)) {
        close(fd);
        return fail(dev, "the first sector did not read");
    }
'''
if source.count(before) != 1 or source.count(write) != 1:
    sys.exit("growroot-mutation: the writer block no longer matches")
path.write_text(source.replace(before, after).replace(write, reread + write))
PY
}


# apply_fault ARGS...: put one fault into its source; with
# RACH2_FAULT_TEXTS set, only check its text and write nothing.
apply_fault()
{
    python3 - "$1" "$2" <<'PYTHON'
import pathlib
import sys
import os
if os.environ.get('RACH2_FAULT_TEXTS'):
    pathlib.Path.write_text = lambda self, *args, **kwargs: 0

path = pathlib.Path(sys.argv[1])
source = path.read_text()
if sys.argv[2] == "zero":
    before = '    printf("growroot: measure:'
    after = '    us = commands = flushes = 0;\n' + before
else:
    before = '    rc = run(tool, arg, part_node);\n'
    marker = '    if (measured(phase, "after")) {\n        return -2;\n    }\n'
    if source.count(marker) != 1:
        sys.exit("growroot-mutation: measurement block no longer matches")
    before += marker
    after = marker + '    rc = run(tool, arg, part_node);\n'
if source.count(before) != 1:
    sys.exit("growroot-mutation: source no longer matches")
path.write_text(source.replace(before, after))
PYTHON
}

faults='zero attribution'
if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1 apply_unchecked_read utils/growroot.c >/dev/null || exit 1
    for mutation in $faults; do
        RACH2_FAULT_TEXTS=1 apply_fault utils/growroot.c "$mutation" >/dev/null || exit 1
    done
    echo 'growroot-mutation: texts: every fault is once in its source'
    exit 0
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-growroot.XXXXXX")
cp utils/growroot.c "$work/growroot.saved"
cp "$work/growroot.saved" "$work/growroot.c"
child=

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    cp "$work/growroot.saved" "$work/growroot.c"
    touch "$work/growroot.c"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work" -I utils tests/host/growroot_hosttest.c utils/gptdisk.c -o "$binary" \
        > "$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 10; exec @ARGV or die "growroot-mutation: exec: $!\n"' \
        "$binary" > "$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ "$phase" = mutated ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'growroot-test: fail: a failed or short writer read makes no write' "$binary.log" &&
            grep -q 'growroot-test: fail: the writer uses the sector it checked without rereading' "$binary.log" || exit 1
    elif [ "$phase" = zero ] || [ "$phase" = attribution ]; then
        [ "$status" -eq 1 ] &&
            grep -q 'growroot-test: fail: phase samples match the independent operation counters' "$binary.log" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "growroot-mutation: result: $phase status $status"
}

echo "growroot-mutation: logs: $work"
run_phase fixed
apply_unchecked_read "$work/growroot.c"
touch "$work/growroot.c"
run_phase mutated
cleanup
run_phase restored
cmp "$work/growroot.saved" "$work/growroot.c"

for mutation in $faults; do
    apply_fault "$work/growroot.c" "$mutation"
    run_phase "$mutation"
    cleanup
    run_phase "restored-$mutation"
    cmp "$work/growroot.saved" "$work/growroot.c"
done
