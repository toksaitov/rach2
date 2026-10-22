#!/bin/sh
# guest.sh: the builds and boots of the proofs in this directory that run a
# kernel and its programs under QEMU, for the architecture ARCH names.
#
# A catalogue's build and test call it from the tree's root, with ${WORK},
# the driver's work directory, as WORK:
#
#   guest.sh make TARGET...          make the targets for the architecture
#   guest.sh copy WORK PATH...       copy each PATH of the architecture's build
#                                    tree, such as rach2/kernel, to WORK/NAME,
#                                    NAME its last part
#   guest.sh image WORK MIB ENTRY... make WORK/test.krffs, a KRFFS image of
#                                    MIB MiB, with each ENTRY, a PATH of the
#                                    build tree or PATH=/NAME, staged at
#                                    /NAME and read back; a PATH that begins
#                                    with / is a file outside the build tree,
#                                    such as a program built into WORK
#   guest.sh boot WORK ARG...        boot WORK/kernel with WORK/test.krffs
#                                    under the harness with ARGs, and print
#                                    its output and the console's log
#   guest.sh run SUITE LINE...       run `tests/run SUITE` and print its
#                                    output; a run that passes must print
#                                    each LINE, or the test fails with
#                                    `guest: fail: SUITE passed without LINE`;
#                                    one that fails prints each text file
#                                    in `evidence/` of each case's workspace
#                                    it kept, after `guest: evidence: FILE`;
#                                    a binary file, such as a disk image, it
#                                    names alone; given TMPDIR, as the
#                                    proof driver gives it, the run keeps
#                                    those workspaces in TMPDIR/failed
#   guest.sh exec WORK COMMAND...    run COMMAND with ARCH the architecture,
#                                    RACH2_QEMU its emulator and ARCH_BUILD
#                                    its build tree relative to the root
#
# A catalogue's artifacts are the copies, whose paths do not depend on the
# architecture. The image is not one, since its bytes depend on more than its
# files. RACH2_QEMU_ACCEL, when set, gives QEMU that accelerator; otherwise
# QEMU emulates the processor, as the harness does.
set -eu
repo=$(pwd)
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
tree=${ARCH_BUILD#"$repo"/}

# qemu WORK: point RACH2_QEMU at the architecture's emulator, through a
# wrapper in WORK that adds the accelerator when RACH2_QEMU_ACCEL names one.
qemu()
{
    RACH2_QEMU=$ARCH_QEMU
    if [ -n "${RACH2_QEMU_ACCEL:-}" ]; then
        printf '#!/bin/sh\nexec %s -accel %s "$@"\n' "$ARCH_QEMU" "$RACH2_QEMU_ACCEL" >"$1/qemu"
        chmod +x "$1/qemu"
        RACH2_QEMU=$1/qemu
    fi
    export RACH2_QEMU
}

usage()
{
    echo 'guest: usage: guest.sh make TARGET... | copy WORK PATH... | image WORK MIB ENTRY... | boot WORK ARG... | run SUITE LINE... | exec WORK COMMAND...' >&2
    exit 2
}

[ "$#" -ge 2 ] || usage
verb=$1
shift
case $verb in
    make)
        exec make -s "$@" "ARCH=$RACH2_ARCH"
        ;;
    copy)
        work=$1
        shift
        for path in "$@"; do
            cp "$tree/$path" "$work/${path##*/}"
        done
        ;;
    image)
        [ "$#" -ge 3 ] || usage
        work=$1 mib=$2
        shift 2
        # These three image tools do not use mkhash's vendored crypt source.
        make -s -C krffs -o libc-check ../build/krffs/mkfs.krffs \
            ../build/krffs/stageimage.krffs ../build/krffs/readfile.krffs "ARCH=$RACH2_ARCH"
        : >"$work/test.manifest"
        for entry in "$@"; do
            case $entry in
                *=/*) path=${entry%%=*} name=${entry#*=} ;;
                *) path=$entry name=/${entry##*/} ;;
            esac
            case $path in
                /*) ;;
                *) path=$tree/$path ;;
            esac
            printf 'file 0755 %s %s\n' "$name" "$path" >>"$work/test.manifest"
        done
        rm -f "$work/test.krffs"
        dd if=/dev/zero of="$work/test.krffs" bs=1048576 count="$mib" 2>/dev/null
        build/krffs/mkfs.krffs "$work/test.krffs"
        build/krffs/stageimage.krffs "$work/test.manifest" "$work/test.krffs"
        while read -r _ _ name path; do
            build/krffs/readfile.krffs "$work/test.krffs" "$name" "$work/staged"
            cmp "$path" "$work/staged"
        done <"$work/test.manifest"
        rm -f "$work/staged"
        ;;
    boot)
        work=$1
        shift
        qemu "$work"
        status=0
        build/tests/system/harness --kernel "$work/kernel" --initrd "$work/test.krffs" \
            --log "$work/console.log" "$@" || status=$?
        cat "$work/console.log" 2>/dev/null || :
        exit "$status"
        ;;
    run)
        suite=$1
        shift
        scratch=
        trap 'rm -rf ${scratch:+"$scratch"}' EXIT
        trap 'exit 129' HUP
        trap 'exit 130' INT
        trap 'exit 143' TERM
        scratch=$(mktemp -d "${TMPDIR:-/tmp}/rach2-guest.XXXXXX")
        if [ -n "${TMPDIR:-}" ]; then
            RACH2_FAILED=$TMPDIR/failed
            export RACH2_FAILED
        fi
        status=0
        ARCH=$RACH2_ARCH tests/run "$suite" >"$scratch/out" 2>&1 </dev/null || status=$?
        cat "$scratch/out"
        if [ "$status" -ne 0 ]; then
            sed -n 's/^run: [^ ]*: [^ ]*: evidence: //p' "$scratch/out" | while read -r kept; do
                for f in "$kept"/evidence/*; do
                    [ -f "$f" ] || continue
                    echo "guest: evidence: $f"
                    if grep -Iq '' "$f"; then
                        cat "$f"
                    fi
                done
            done
        fi
        if [ "$status" -eq 0 ]; then
            for line in "$@"; do
                if ! grep -Fq -- "$line" "$scratch/out"; then
                    echo "guest: fail: $suite passed without $line"
                    status=1
                fi
            done
        fi
        exit "$status"
        ;;
    exec)
        [ "$#" -ge 2 ] || usage
        work=$1
        shift
        qemu "$work"
        ARCH=$RACH2_ARCH ARCH_BUILD=$tree
        export ARCH ARCH_BUILD
        exec "$@"
        ;;
    *)
        usage
        ;;
esac
