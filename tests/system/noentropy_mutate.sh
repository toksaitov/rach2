#!/bin/sh
# Prove the fail-closed steps of `tests/run noentropy` with compiled faults, on
# the kernel and suite of the architecture ARCH names.
#
# Three faults are put into the kernel in turn, each alone: `urandom` lets
# the random devices answer a read while the generator is untrusted,
# `random` gives /dev/random the operations of /dev/zero, and `insecure`
# refuses getrandom's GRND_INSECURE there. Under each, every step of the suite
# runs, make told to go on past a failure, so that each step is seen to catch
# the fault or to pass it: `urandom` must fail the random-number probe's step
# on /dev/urandom and let dropbearkey make a key, which fails the key
# generator's step; `random` must fail the probe's step on /dev/random; and
# `insecure` must fail the probe's step on GRND_INSECURE. The unmodified suite
# passes first, through tests/run, and again after each source is restored,
# touched and rebuilt, with the kernel's bytes those of the first build.
#
# Usage: sh tests/system/noentropy_mutate.sh [--direct] [FAULT...]
#   --direct runs the unmodified phases with `make run-noentropy` instead of
#   `tests/run noentropy`. FAULT is urandom, random or insecure; the default
#   is all three.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
RACH2_QEMU=$ARCH_QEMU
export RACH2_QEMU ARCH_BUILD
tree=build$ARCH_SUFFIX
runner=suite
if [ "${1-}" = --direct ]; then
    runner=direct
    shift
fi
if [ "$#" -eq 0 ]; then
    set -- urandom random insecure
fi
for fault do
    case "$fault" in
        urandom|random|insecure) ;;
        *) echo "noentropy-mutation: unknown fault: $fault" >&2; exit 2 ;;
    esac
done
. "$repo/tests/system/runlock.sh"
runlock_hold "$repo" noentropy-mutation "$repo/tests/system/noentropy_mutate.sh" "$@"
mkdir -p build
work=$(mktemp -d "$repo/build/noentropy-proof.XXXXXX")
for source in rach2/sys/fs/devfs.c rach2/sys/syscall.c; do
    cp "$source" "$work/$(basename "$source").saved"
done
cc -O2 -Wall -Wextra -Werror tests/system/runner/runutil.c -o "$work/runutil"
child=
changed=

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

force_rebuild()
{
    rm -f "$tree"/rach2/sys/fs/devfs.o "$tree"/rach2/sys/syscall.o "$tree"/rach2/kernel
}

restore()
{
    if [ -n "$changed" ]; then
        cp "$work/$(basename "$changed").saved" "$changed"
        touch "$changed"
        changed=
    fi
}

cleanup()
{
    stop_child
    if [ -n "$changed" ]; then
        restore
        force_rebuild
        make -C rach2 "ARCH=$RACH2_ARCH" >"$work/cleanup.build.log" 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# The kernel with the sources as they stand, built first and on its own so
# that a fault that does not compile stops the proof here, and its checksum
# kept beside the phase's logs.
build_kernel()
{
    force_rebuild
    make -C rach2 "ARCH=$RACH2_ARCH" >"$work/$1.build.log" 2>&1 ||
        { cat "$work/$1.build.log"; exit 1; }
    cksum "$tree"/rach2/kernel >"$work/$1.kernel"
}

# One run of the suite in a process group of its own, its output in the
# phase's log and its status in $status, and the machines' consoles kept.
run_suite()
{
    phase=$1
    shift
    rm -f "$tree"/images/noentropy-rng.log "$tree"/images/noentropy-keygen.log
    "$work/runutil" group "$@" >"$work/$phase.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    for kind in rng keygen; do
        log="$tree"/images/noentropy-$kind.log
        [ ! -f "$log" ] || cp "$log" "$work/$phase.$kind.log"
    done
    cat "$work/$phase.log"
}

# The unmodified suite: every step must pass.
pass_phase()
{
    build_kernel "$1"
    if [ "$runner" = direct ]; then
        run_suite "$1" make run-noentropy "ARCH=$RACH2_ARCH"
    else
        run_suite "$1" env "ARCH=$RACH2_ARCH" tests/run noentropy
    fi
    [ "$status" -eq 0 ] || { echo "noentropy-mutation: $1 failed" >&2; exit 1; }
    echo "noentropy-mutation: result: $1 status $status"
}

# A fault compiled in: the kernel must differ from the first build, and with
# make going on past a failure every step runs and each says what it saw.
fault_phase()
{
    phase=$1
    build_kernel "$phase"
    if cmp -s "$work/fixed.kernel" "$work/$phase.kernel"; then
        echo "noentropy-mutation: the $phase fault did not change the kernel" >&2
        exit 1
    fi
    run_suite "$phase" make -i run-noentropy "ARCH=$RACH2_ARCH"
    grep -Fq "HARNESS: saw 'RNGPROBE FAIL', which must not appear" "$work/$phase.log" || {
        echo "noentropy-mutation: fault passed the probe's step: $phase" >&2; exit 1;
    }
    case $phase in
        urandom) reason='urandom not fail-closed' ;;
        random) reason='random not fail-closed' ;;
        insecure) reason='GRND_INSECURE refused' ;;
    esac
    grep -Fq "RNGPROBE FAIL ($reason)" "$work/$phase.rng.log" || {
        echo "noentropy-mutation: wrong failure: $phase" >&2; exit 1;
    }
    if [ "$phase" = urandom ]; then
        grep -Fq "HARNESS: saw 'KEYGEN:MADE', which must not appear" "$work/$phase.log" || {
            echo "noentropy-mutation: fault passed the key generator's step: $phase" >&2
            exit 1
        }
    fi
    echo "noentropy-mutation: result: $phase failed its steps"
}

echo "noentropy-mutation: logs: $work"
pass_phase fixed
for fault do
    case "$fault" in
        urandom|random) changed=rach2/sys/fs/devfs.c ;;
        insecure) changed=rach2/sys/syscall.c ;;
    esac
    python3 - "$fault" "$changed" <<'PY'
from pathlib import Path
import sys

fault, name = sys.argv[1:]
path = Path(name)
text = path.read_text()
if fault == 'urandom':
    start = text.index('static int32_t dev_rand_read(')
    end = text.index('\n}\n', start)
    body = text[start:end]
    assert body.count('if (!krandom_is_trusted()) {') == 1
    text = text[:start] + body.replace('if (!krandom_is_trusted()) {', 'if (0) {') + text[end:]
elif fault == 'random':
    old = '{"random", 1, 8, 0, 0666, &rand_ops}'
    assert text.count(old) == 1
    text = text.replace(old, '{"random", 1, 8, 0, 0666, &zero_ops}')
else:
    old = 'if (!krandom_is_trusted() && !(flags & GRND_INSECURE)) {'
    assert text.count(old) == 1
    text = text.replace(old, 'if (!krandom_is_trusted()) {')
path.write_text(text)
PY
    touch "$changed"
    fault_phase "$fault"
    restore
    pass_phase "$fault-restored"
    cmp "$work/fixed.kernel" "$work/$fault-restored.kernel"
done
for source in rach2/sys/fs/devfs.c rach2/sys/syscall.c; do
    cmp "$source" "$work/$(basename "$source").saved"
done
echo "noentropy-mutation: pass: $# compiled faults failed their steps; restored runs passed"
