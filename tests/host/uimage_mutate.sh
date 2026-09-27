#!/bin/sh
# Prove the checks of uimage_hosttest.c against faults in a private copy of
# the initial-stack builder.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build
work=$(mktemp -d "$repo/build/uimage-proof.XXXXXX")
child=
cp rach2/sys/proc/uimage.c "$work/uimage.saved"
cp "$work/uimage.saved" "$work/uimage.c"

restore()
{
    cp "$work/uimage.saved" "$work/uimage.c"
    touch "$work/uimage.c"
}

cleanup()
{
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        child=
    fi
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# run_phase PHASE STATUS DETAIL: compile the test against the private copy
# and run it; it must exit with STATUS and print DETAIL.
run_phase()
{
    phase=$1
    expected=$2
    detail=$3
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -g -O1 -ffreestanding -DRACH2_HOSTTEST \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I rach2/sys -I rach2/sys/lib -I rach2/sys/proc \
        tests/host/uimage_hosttest.c "$work/uimage.c" rach2/sys/lib/kstring.c \
        rach2/sys/lib/kctypes.c tests/host/compat/kheap_host.c \
        -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/uimage.c" "$binary" >"$binary.artifacts"
    perl -e 'alarm 20; exec @ARGV or die "uimage-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    child=
    cat "$binary.log"
    [ "$status" -eq "$expected" ] || exit 1
    grep -F "$detail" "$binary.log" >/dev/null || exit 1
    echo "uimage-mutation: result: $phase status $status"
}

echo "uimage-mutation: logs: $work"
run_phase fixed 0 'uimage-test: pass: every row'
for mutation in budget-quarter budget-cap budget-floor pointers-argv0 pointers-edge \
    string-max room-edge room-check nul zero random-align esp-align clktck secure \
    argv-null envp-null drop-clear drop-used map-address map-flags frames-cap \
    release-given release-none fault; do
    python3 - "$work" "$mutation" <<'PY'
from pathlib import Path
import sys
work = Path(sys.argv[1])
changes = {
    'budget-quarter': [('uint32_t limit = stack_limit / 4u;',
                        'uint32_t limit = stack_limit / 2u;')],
    'budget-cap': [('if (limit > UIMAGE_ARGS_MAX) {', 'if (0) {')],
    'budget-floor': [('if (limit < UIMAGE_ARGS_MIN) {', 'if (0) {')],
    'pointers-argv0': [('pointers = ((argc > 0 ? argc : 1u) + envc) * 4u;',
                        'pointers = (argc + envc) * 4u;')],
    'pointers-edge': [('if (pointers >= limit) {', 'if (pointers > limit) {')],
    'string-max': [('int n = strnlen_user(s, UIMAGE_STRING_MAX);',
                    'int n = strnlen_user(s, UIMAGE_STRING_MAX + 1u);'),
                   ('if (len > UIMAGE_STRING_MAX || len > u->room - u->used) {',
                    'if (len > UIMAGE_STRING_MAX + 1u || len > u->room - u->used) {')],
    'room-edge': [('len > u->room - u->used', 'len >= u->room - u->used')],
    'room-check': [('if (len > UIMAGE_STRING_MAX || len > u->room - u->used) {',
                    'if (len > UIMAGE_STRING_MAX) {')],
    'nul': [("        if (done == len) {\n            dst[chunk - 1u] = '\\0';\n        }\n",
             '')],
    'zero': [('if (paging_zero_frame(frame) != 0) {', 'if (0) {')],
    'random-align': [('return (u->top - u->used - 16u) & ~15u;',
                      'return u->top - u->used - 16u;')],
    'esp-align': [('u->esp = (uimage_random_at(u) - words * 4u) & ~15u;',
                   'u->esp = (uimage_random_at(u) - words * 4u) & ~3u;')],
    'clktck': [('aux[11] = 100;', 'aux[11] = 1000;')],
    'secure': [('aux[13] = secure ? 1u : 0u;', 'aux[13] = 0;')],
    'argv-null': [('if (i == argc) {', 'if (i == argc + 1u) {')],
    'envp-null': [('    if (rc == 0) {\n        rc = uimage_put(&out, &at, 0);\n    }\n'
                   '    for (i = 0; i < 2u * UIMAGE_AUXV',
                   '    for (i = 0; i < 2u * UIMAGE_AUXV')],
    'drop-clear': [('        *p = 0;\n        addr++;', '        addr++;')],
    'drop-used': [('u->used = u->top - addr;', 'u->used = u->top - addr + 1u;')],
    'map-address': [('uint32_t page = u->top - (u->given + 1u) * PAGE_SIZE;',
                     'uint32_t page = u->top - u->given * PAGE_SIZE;')],
    'map-flags': [('u->frames[u->given], 0x7u)', 'u->frames[u->given], 0x5u)')],
    'frames-cap': [('u->cap = limit / PAGE_SIZE + 4u;', 'u->cap = limit / PAGE_SIZE;')],
    'release-given': [('for (i = u->given; i < u->nframes; i++) {',
                       'for (i = 0; i < u->nframes; i++) {')],
    'release-none': [('        free_frame(u->frames[i]);', '        (void)u->frames[i];')],
    'fault': [('    if (n == -EFAULT) {\n        return -EFAULT;\n    }\n', '')],
}
path = work / 'uimage.c'
text = path.read_text()
for old, new in changes[sys.argv[2]]:
    if text.count(old) != 1:
        raise SystemExit('uimage-mutation: ' + sys.argv[2] + ': the text to change is not there once')
    text = text.replace(old, new, 1)
path.write_text(text)
PY
    case $mutation in
        budget-quarter) detail='budget: 1 MiB gives 256 KiB' ;;
        budget-cap) detail='budget: 64 MiB gives 6 MiB' ;;
        budget-floor) detail='budget: a limit of 0 gives 128 KiB' ;;
        pointers-argv0) detail='pointers: an empty argv has a pointer set aside for argv[0]' ;;
        pointers-edge) detail='pointers: pointers that take the whole budget are refused' ;;
        string-max) detail='string: a string of 128 KiB and a NUL is refused' ;;
        room-edge) detail='edge: the strings may fill the budget to its last byte' ;;
        room-check) detail='edge: a string a byte past the budget is refused' ;;
        nul) detail='string: a string that grew after it was measured ends where it was measured' ;;
        zero) detail='one page: the bytes below the stack pointer are zero' ;;
        random-align) detail='AT_RANDOM is aligned to 16 bytes' ;;
        esp-align) detail='one page: the stack pointer is aligned to 16 bytes' ;;
        clktck) detail='the auxiliary vector has the entries execve gives' ;;
        secure) detail='the auxiliary vector has the entries execve gives' ;;
        argv-null) detail='one page: argv ends in NULL' ;;
        envp-null) detail='one page: envp ends in NULL' ;;
        drop-clear) detail='script: the bytes of argv[0] are cleared' ;;
        drop-used) detail='script: argv[0] is dropped' ;;
        map-address) detail='one page: the page is mapped user and writable below the top' ;;
        map-flags) detail='one page: the page is mapped user and writable below the top' ;;
        frames-cap) detail='edge: the information block below a full budget has frames' ;;
        release-given) detail='a frame is given back once, after it was taken' ;;
        release-none) detail='failures: the frames not mapped are given back' ;;
        fault) detail='string: an unreadable string is -EFAULT' ;;
    esac
    run_phase "$mutation" 1 "$detail"
    restore
    run_phase "$mutation-restored" 0 'uimage-test: pass: every row'
done
cmp "$work/uimage.saved" "$work/uimage.c"
