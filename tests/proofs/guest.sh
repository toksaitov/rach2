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
#                                    /NAME and read back
#   guest.sh boot WORK ARG...        boot WORK/kernel with WORK/test.krffs
#                                    under the harness with ARGs, and print
#                                    its output and the console's log
#   guest.sh default PROBE           boot the default suite and print its
#                                    output and the console's log, with
#                                    `guest: fail: PROBE did not pass` when
#                                    the runner did not pass PROBE
#   guest.sh run SUITE LINE...       run `tests/run SUITE` and print its
#                                    output; a run that passes must print
#                                    each LINE, or the test fails with
#                                    `guest: fail: SUITE passed without LINE`
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
    echo 'guest: usage: guest.sh make TARGET... | copy WORK PATH... | image WORK MIB ENTRY... | boot WORK ARG... | default PROBE | run SUITE LINE...' >&2
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
            printf 'file 0755 %s %s\n' "$name" "$tree/$path" >>"$work/test.manifest"
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
    default)
        [ "$#" -eq 1 ] || usage
        probe=$1
        log=$tree/images/default-test.log
        rm -f "$log"
        scratch=
        trap 'rm -rf ${scratch:+"$scratch"}' EXIT
        trap 'exit 129' HUP
        trap 'exit 130' INT
        trap 'exit 143' TERM
        scratch=$(mktemp -d "${TMPDIR:-/tmp}/rach2-guest.XXXXXX")
        qemu "$scratch"
        status=0
        make run-default "ARCH=$RACH2_ARCH" "RACH2_QEMU=$RACH2_QEMU" >"$scratch/out" 2>&1 \
            </dev/null || status=$?
        cat "$scratch/out"
        cat "$log" 2>/dev/null || :
        if ! grep -q '^runtests: [0-9]* of [0-9]* probes' "$scratch/out"; then
            echo 'guest: fail: the suite did not run'
            [ "$status" -ne 0 ] || status=1
        elif ! grep -Eq "^  (musl/)?$probe +.* PASS\$" "$scratch/out"; then
            echo "guest: fail: $probe did not pass"
            [ "$status" -ne 0 ] || status=1
        fi
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
        status=0
        ARCH=$RACH2_ARCH tests/run "$suite" >"$scratch/out" 2>&1 </dev/null || status=$?
        cat "$scratch/out"
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
    *)
        usage
        ;;
esac
