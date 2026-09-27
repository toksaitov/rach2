#!/bin/sh
# Compile synthetic-file regressions before, during and after each fault:
# the ptmx dispatch before the flag checks, the text's terminator written
# with no buffer, a read of a process's arguments that holds no reference to
# the address space or reads the process's record after the first read,
# which may sleep while the process is reaped, and an oom_score_adj write
# that takes a value out of range, lowers the value below the floor without
# privilege, or leaves the floor where a privileged write found it.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/synth-open-proof.XXXXXX")
child=
cp -R rach2/sys "$work/sys"
for component in devfs procfs; do
    cp "$work/sys/fs/$component.c" "$work/$component.saved"
done

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    for component in devfs procfs; do
        cp "$work/$component.saved" "$work/sys/fs/$component.c"
        touch "$work/sys/fs/$component.c"
    done
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    test=$1
    phase=$2
    expected=$3
    message=$4
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 \
        -DRACH2_HOSTTEST -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/sys" -I "$work/sys/lib" -I "$work/sys/proc" \
        "tests/host/${test}_hosttest.c" "$work/sys/lib/kstring.c" \
        "$work/sys/lib/kctypes.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/sys/fs/$test.c" "$binary" >"$binary.cksum"
    UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=1 perl -e 'alarm 10; exec @ARGV or die "synth-open-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "synth-open-mutation: result: $test $phase status $status"
}

echo "synth-open-mutation: logs: $work"
for case in devfs:ptmx procfs:terminator procfs:unheld-space procfs:record-after-sleep \
    procfs:oom-range procfs:oom-floor procfs:oom-floor-kept; do
    test=${case%%:*}
    fault=${case#*:}
    run_phase "$test" "$fault-fixed" 0 "$test-hosttest: result: pass:"
    python3 - "$work/sys/fs/$test.c" "$fault" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
if sys.argv[2] == 'ptmx':
    old = '    if (name_eq(name, "ptmx")) {\n        return pty_open_master(flags);\n    }\n'
    assert source.count(old) == 1
    source = source.replace(old, '')
    at = source.index('    dev = devfs_find(name);', source.index('int devfs_open('))
    source = source[:at] + old + source[at:]
else:
    changes = {
        'terminator': [("    if (!t->nomem) {\n        t->buf[t->len] = '\\0';\n    }",
                        "    t->buf[t->len] = '\\0';")],
        'unheld-space': [("    mm_get(mm);\n    while (start < end) {", "    while (start < end) {"),
                         ("        start += n;\n    }\n    procfs_mm_put(mm);\n}",
                          "        start += n;\n    }\n}")],
        'record-after-sleep': [("        if (paging_access(mm, start, chunk, n, 0) != 0) {\n"
                                "            break;\n        }\n        for (i = 0; i < n; i++) {",
                                "        if (paging_access(p->mm, start, chunk, n, 0) != 0) {\n"
                                "            break;\n        }\n        for (i = 0; i < n; i++) {")],
        'oom-range': [("    if (v < OOM_SCORE_ADJ_MIN || v > OOM_SCORE_ADJ_MAX) {", "    if (0) {")],
        'oom-floor': [("    if (v < p->tg->oom_score_adj_min && !proc_privileged(current_proc)) {",
                       "    if (0) {")],
        'oom-floor-kept': [("    p->tg->oom_score_adj = v;\n    if (proc_privileged(current_proc)) {\n"
                            "        p->tg->oom_score_adj_min = v;\n    }\n",
                            "    p->tg->oom_score_adj = v;\n")],
    }
    for old, new in changes[sys.argv[2]]:
        assert source.count(old) == 1
        source = source.replace(old, new)
path.write_text(source)
PY
    case $fault in
        ptmx) message='ptmx exclusive create returns EEXIST before dispatch' ;;
        terminator) message='runtime error: store to null pointer' ;;
        unheld-space | record-after-sleep) message='heap-use-after-free' ;;
        oom-range) message='a value past -1000 to 1000 is EINVAL and changes nothing' ;;
        oom-floor) message='and may not lower it below the floor' ;;
        oom-floor-kept) message='a privileged write sets the floor' ;;
    esac
    run_phase "$test" "$fault-mutated" 1 "$message"
    cleanup
    run_phase "$test" "$fault-restored" 0 "$test-hosttest: result: pass:"
    cmp "$work/$test.saved" "$work/sys/fs/$test.c"
done
