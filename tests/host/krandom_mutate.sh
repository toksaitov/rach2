#!/bin/sh
# Prove krandom_hosttest.c with faults of a private copy of
# rach2/sys/lib/krandom.c, one at a time: ChaCha20 with nineteen rounds, a
# block counter that does not carry, a key that keeps four of its words as
# they came, RDRAND not trusted, and RDSEED given 31 attempts at a word. Each
# must fail its row, and the restored copy must pass.
#
# Usage: sh tests/host/krandom_mutate.sh [--texts]
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"

# fault ACTION FILE NAME: with ACTION `check`, exit unless the text fault NAME
# replaces is once in FILE; with `apply`, replace it there. With NAME `list`,
# print each fault's name and the row it must fail, separated by a tab.
fault()
{
    python3 -I - "$@" <<'PY'
import sys
from pathlib import Path

ROUNDS = '    for (i = 0; i < 10; i++) {\n        QR(x[0], x[4], x[8], x[12]);'
FAULTS = {
    'rounds': (ROUNDS, ROUNDS.replace('i < 10', 'i < 9'),
               "chacha_block gives RFC 8439's block 2.3.2"),
    'carry': ('            ++cc_state[13];\n', '',
              'the counter carries into its high word'),
    'expand': ('    key[4] = a ^ 0x9e3779b9u;\n', '    key[4] = a;\n',
               'krandom_reseed expands four words to its eight-word key'),
    'rdrand-trust': ('return cc_src == KRANDOM_SRC_RDSEED || cc_src == KRANDOM_SRC_RDRAND;',
                     'return cc_src == KRANDOM_SRC_RDSEED;',
                     'RDRAND is the trusted source when RDSEED stops part way'),
    'rdseed-tries': ('static int rdseed32(uint32_t *out)\n{\n    int tries;\n\n'
                     '    for (tries = 0; tries < 32; tries++) {',
                     'static int rdseed32(uint32_t *out)\n{\n    int tries;\n\n'
                     '    for (tries = 0; tries < 31; tries++) {',
                     "a word of RDSEED's on its 32nd attempt is taken"),
}
action, path, name = sys.argv[1:4]
if name == 'list':
    for key, (_, _, row) in FAULTS.items():
        print('%s\t%s' % (key, row))
    sys.exit(0)
old, new, _ = FAULTS[name]
p = Path(path)
text = p.read_text()
if text.count(old) != 1:
    sys.exit('krandom-mutation: the text of %s is not once in %s' % (name, path))
if action == 'apply':
    p.write_text(text.replace(old, new))
PY
}

if [ "${1:-}" = --texts ]; then
    fault check rach2/sys/lib/krandom.c list | while IFS='	' read -r name _; do
        fault check rach2/sys/lib/krandom.c "$name"
    done
    echo 'krandom-mutation: texts: every fault is once in its source'
    exit 0
fi

. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" krandom-mutation "$repo/tests/host/krandom_mutate.sh" "$@"
work=
child=
stopped=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'stopped=1; exit 129' HUP
trap 'stopped=1; exit 130' INT
trap 'stopped=1; exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-krandom.XXXXXX")
cp -R rach2/sys "$work/sys"
source="$work/sys/lib/krandom.c"
cp "$source" "$work/krandom.saved"

# cleanup: stop the running test and put the saved krandom.c back; the script
# calls it after each fault, and the exit trap calls it, then removes $work
# if the script exits 0 or a signal stops it.
cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    cp "$work/krandom.saved" "$source"
}

st=0
trap 'st=$?; cleanup; { [ "$st" -ne 0 ] && [ -z "${stopped:-}" ]; } || [ -z "$work" ] || rm -rf "$work"' EXIT

# run_phase PHASE [ROW]: compile krandom_hosttest.c over the copy as
# $work/PHASE and run it for at most 10 seconds. With ROW the test must fail
# and name that row; without, it must pass. Exit if not.
run_phase()
{
    phase=$1
    row=${2:-}
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -ffreestanding \
        -I "$work/sys" -I "$work/sys/arch/x86" -I "$work/sys/arch/i686" -I "$work/sys/lib" \
        tests/host/krandom_hosttest.c -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    perl -e 'alarm 10; exec @ARGV or die "krandom-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    if [ -n "$row" ]; then
        [ "$status" -ne 0 ] && grep '^krandom-test: fail: ' "$binary.log" |
            grep -qF ": $row" || exit 1
    else
        [ "$status" -eq 0 ] || exit 1
    fi
    echo "krandom-mutation: result: $phase status $status"
}

echo "krandom-mutation: logs: $work"
run_phase fixed
fault check "$source" list >"$work/faults"
while IFS='	' read -r name row; do
    fault apply "$source" "$name"
    run_phase "$name" "$row"
    cleanup
done <"$work/faults"
run_phase restored
cmp "$work/krandom.saved" "$source"
