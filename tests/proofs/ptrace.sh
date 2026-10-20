#!/bin/sh
# ptrace.sh: the builds and the test of the proofs ptrace-debug-*.json and
# ptrace-debug-fixture-*.json, which run ptracemusl's rows one at a time, by
# name, from a probe built with PTRACE_DEBUG_ENTRY, for the architecture ARCH
# names.
#
# A catalogue's build and test call it from the tree's root:
#
#   ptrace.sh build WORK          make the kernel and copy it to WORK/kernel
#   ptrace.sh fixture SRC WORK    build WORK/kernel in a private copy of rach2/
#                                 and mk/ under WORK/tree, with the sources the
#                                 faults change taken from SRC when SRC has
#                                 them, and the debug-entry fixture of SRC's
#                                 tests/proofs/ptrace_fixture.py put in
#   ptrace.sh rows WORK ROW...    boot the kernel once for each ROW and run
#                                 `ptracemusl ROW`, which must pass, print its
#                                 result line when the row has one, and exit 0
#                                 to the kernel shell
#
# Both builds then compile WORK/ptracemusl with PTRACE_DEBUG_ENTRY, which gives
# the probe its delayed and delayed-poll rows, and stage it in WORK/test.krffs.
set -eu
repo=$(pwd)
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
tree=${ARCH_BUILD#"$repo"/}
# The sources a fault of the proofs or the fixture changes.
sources='rach2/sys/syscall.c rach2/sys/syscall_proc.c rach2/sys/arch/x86/debugreg.c
    rach2/sys/arch/x86_64/idt.c rach2/sys/arch/x86_64/isr_handler.s
    rach2/sys/arch/i686/isr_handler.s'

usage()
{
    echo 'ptrace: usage: ptrace.sh build WORK | fixture SRC WORK | rows WORK ROW...' >&2
    exit 2
}

# probe WORK: compile ptracemusl with its debug-entry rows and stage it alone.
probe()
{
    vendor/musl/muslgcc -DPTRACE_DEBUG_ENTRY -O2 -Wall -Wextra -Werror \
        -Wl,-z,noexecstack -o "$1/ptracemusl" tests/musl/ptracemusl.c
    sh tests/proofs/guest.sh image "$1" 2 "$1/ptracemusl"
}

[ "$#" -ge 2 ] || usage
verb=$1
shift
case $verb in
    build)
        [ "$#" -eq 1 ] || usage
        sh tests/proofs/guest.sh make rach2 musl build/tests/system/harness
        sh tests/proofs/guest.sh copy "$1" rach2/kernel
        probe "$1"
        ;;
    fixture)
        [ "$#" -eq 2 ] || usage
        src=$1 work=$2
        copy=$work/tree
        sh tests/proofs/guest.sh make musl build/tests/system/harness \
            build/tools/inspect/elfcheck
        # The private tree has what the kernel's own Makefile reads: rach2/
        # and mk/. Its link asks the directory above for the ELF checker, so
        # that directory gets a copy of the checker and a Makefile with
        # nothing to rebuild.
        if [ ! -d "$copy" ]; then
            mkdir -p "$copy/build/tools/inspect"
            cp -R rach2 mk "$copy/"
            printf 'build/tools/inspect/elfcheck: ;\n' >"$copy/Makefile"
        fi
        cp build/tools/inspect/elfcheck "$copy/build/tools/inspect/elfcheck"
        for file in $sources; do
            if [ -f "$src/$file" ]; then
                cp "$src/$file" "$copy/$file"
            else
                cp "$file" "$copy/$file"
            fi
        done
        python3 "$src/tests/proofs/ptrace_fixture.py" "$copy" "$RACH2_ARCH"
        make -s -j8 -C "$copy/rach2" "ARCH=$RACH2_ARCH"
        cp "$copy/$tree/rach2/kernel" "$work/kernel"
        probe "$work"
        ;;
    rows)
        work=$1
        shift
        [ "$#" -ge 1 ] || usage
        status=0
        for row in "$@"; do
            case $row in
                delayed|delayed-poll) result="expect=ptracemusl: result: $row: kernel entry" ;;
                *) result= ;;
            esac
            sh tests/proofs/guest.sh boot "$work" --net hubport,id=net0,hubid=0 \
                --qemu-arg=-cpu --qemu-arg=max --append rach2.rescue \
                --timeout 40 --steptmo 15 --ok 'PTRACEMUSL OK' --never 'PTRACEMUSL FAIL' \
                "expect=Type 'help'" "send=exec /ptracemusl $row" ${result:+"$result"} \
                'expect=PTRACEMUSL' 'expect=exited with status 0, returning to kernel shell' ||
                status=1
        done
        exit "$status"
        ;;
    *)
        usage
        ;;
esac
