#!/bin/sh
# Prove etude_proof_hosttest.c with faults in private copies of the proof
# driver: a fault that does not reach the artifacts taken for one that does,
# a restored artifact that differs from the base's accepted, a test that
# passes with a fault, one that exits with another status, one without its
# line and one that fails a check the fault does not name each taken as a
# fault found, a build that changes its source unseen, an edit applied to a
# text found twice, a shared tree taken as held when it is not, a source
# left no newer than an artifact whose time is ahead, a stopped proof that
# keeps its work or leaves its faults in the tree, commands given the
# driver's TMPDIR, a relative work path left relative, a work that keeps an
# unwritable directory after a pass, and a work left after a pass with no
# line that names it must each be found.
#
# Usage: sh tests/host/etude_proof_mutate.sh [--texts]
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# Change the unique fault text in a private source, or just locate it.
apply_fault()
{
    python3 - "$1" "$2" <<'PY'
from pathlib import Path
import os
import sys
path = Path(sys.argv[1])
source = path.read_text()
faults = {
    'unreached': ('    if (kind == FAULT && same == r->p->nartifacts)\n',
                  '    if (0)\n'),
    'restored': ('        else if (kind == RESTORED)\n', '        else if (0)\n'),
    'passed': ('    if (x->value == 0)\n        return fail(r, "the test passed with the fault");\n',
               '    if (0)\n        return fail(r, "the test passed with the fault");\n'),
    'status': ('    if (f->status != 0 && x->value != f->status)\n', '    if (0)\n'),
    'lines': ('        if (strstr(x->out, f->lines[i]) == NULL && strstr(x->err, f->lines[i]) == NULL)\n',
              '        if (0)\n'),
    'only': ('    if (f->only)\n', '    if (0)\n'),
    'kept': ('        if (!has_text(r->src[i].path, r->src[i].cur, r->src[i].curlen))\n',
             '        if (0)\n'),
    'once': ('    if (count != 1)\n        return count;\n',
             '    if (count == 0)\n        return count;\n    count = 1;\n'),
    'held': ('    return ok && (kill((pid_t)pid, 0) == 0 || errno == EPERM);\n',
             '    return 1;\n'),
    'newer': ('    if (st.st_mtime > newest)\n        return 0;\n',
              '    if (1)\n        return 0;\n'),
    'stop-work': ('((status == 0 && !r->o->keep) || status == ETUDE_PROOF_STOPPED)',
                  '(status == 0 && !r->o->keep)'),
    'stop-restore': ('        if (!s->dirty)\n            continue;\n',
                     '        if (!s->dirty || *r->stop)\n            continue;\n'),
    'tmpdir': ('        spec.envp = env;\n', '        spec.envp = environ;\n'),
    'absolute': ("    if (r->work != NULL && r->work[0] != '/') {\n", '    if (0) {\n'),
    'writable': ('    chmod(path, (st.st_mode & 07777) | S_IRWXU);\n', ''),
    'unremoved': ('        if (remove_tree(r->work) != 0)\n',
                  '        if (remove_tree(r->work) != 0 && 0)\n'),
}
old, new = faults[sys.argv[2]]
if source.count(old) != 1:
    raise SystemExit('etude-proof-mutation: fault text is not unique: ' + sys.argv[2])
if not os.environ.get('RACH2_FAULT_TEXTS'):
    path.write_text(source.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    RACH2_FAULT_TEXTS=1
    export RACH2_FAULT_TEXTS
    apply_fault tools/etude/proof.c once
    for fault in unreached restored passed status lines only kept held newer stop-work \
        stop-restore tmpdir absolute writable unremoved; do
        apply_fault tools/etude/posix/proof.c "$fault"
    done
    echo 'etude-proof-mutation: texts: all sixteen faults found once'
    exit 0
fi
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" etude-proof-mutation "$repo/tests/host/etude_proof_mutate.sh" "$@"
work=
child=

# Stop a test before removing its private sources and evidence, with the
# unwritable directories a faulty driver left in the test's fixtures.
cleanup()
{
    status=$?
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    if [ "$status" -eq 0 ] || [ -n "${stopped:-}" ]; then
        [ -z "$work" ] || { chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"; }
    else
        echo "etude-proof-mutation: evidence: $work" >&2
    fi
}
trap cleanup EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-etude-proof-proof.XXXXXX")

# Copy the sources the faults go into afresh.
restore()
{
    mkdir -p "$work/posix"
    cp tools/etude/proof.c "$work/proof.c"
    cp tools/etude/posix/proof.c "$work/posix/proof.c"
}

# Build and run the real host test against the private sources under the
# sanitizers, with its fixtures in the scratch, and require `expected` as its
# status.
run_phase()
{
    phase=$1
    expected=$2
    "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
        -fno-sanitize-recover=all -I tests/host/compat -I utils -I tools/etude/include \
        -I tools/etude -I tools/etude/posix tests/host/etude_proof_hosttest.c "$work/proof.c" \
        "$work/posix/proof.c" tools/etude/json.c tools/etude/schema.c \
        tools/etude/posix/store.c tools/etude/capture.c tools/etude/posix/exec.c \
        tools/etude/posix/keeper.c tools/etude/posix/process.c utils/sha256.c \
        -o "$work/$phase" >"$work/$phase.compile.log" 2>&1 || {
            cat "$work/$phase.compile.log" >&2; exit 1;
        }
    status=0
    TMPDIR=$work perl -e 'alarm 120; exec @ARGV or die "etude-proof-mutation: exec: $!\n"' \
        "$work/$phase" >"$work/$phase.log" 2>&1 &
    child=$!
    wait "$child" || status=$?
    child=
    cat "$work/$phase.log"
    [ "$status" -eq "$expected" ] || exit 1
    echo "etude-proof-mutation: result: $phase status $status"
}

# Plant `fault` in `file` of the copy, require the test to fail with a line
# that has `text`, and restore the copy.
prove()
{
    restore
    apply_fault "$work/$1" "$2"
    run_phase "$2-mutated" 1
    grep -q "^etude-proof-test: fail: .*$3" "$work/$2-mutated.log"
}

restore
run_phase fixed 0
prove posix/proof.c unreached ': unreached: '
prove posix/proof.c restored ': leftover: '
prove posix/proof.c passed ': missed: '
prove posix/proof.c status ': status: '
prove posix/proof.c lines ': line: '
prove posix/proof.c only ': only: '
prove posix/proof.c kept ': rewrite: '
prove proof.c once ': found: '
prove posix/proof.c held 'shared: a proof whose tree is not held is refused'
prove posix/proof.c newer 'sound: a build that judges by time stamps ahead of the clock'
prove posix/proof.c stop-work 'stopped: the work is removed'
prove posix/proof.c stop-restore 'stopped: the tree is restored'
prove posix/proof.c tmpdir "scratch: the faulty test's TMPDIR is its own in the work"
prove posix/proof.c absolute 'relative: a relative work directory gives the commands an absolute'
prove posix/proof.c writable 'scratch: the work is removed after a pass, with an unwritable'
prove posix/proof.c unremoved 'unremoved: a work that cannot be removed after a pass is named'
restore
run_phase restored 0
