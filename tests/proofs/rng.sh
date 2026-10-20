#!/bin/sh
# rng.sh: the build and the test of the proofs rng-probe.json and
# rng-kernel.json, which boot rngprobe under etude's processor profile max and
# judge its report records as etude does, for the architecture ARCH names.
#
# A catalogue's build and test call it from the tree's root:
#
#   rng.sh build SRC WORK   make the kernel and copy it to WORK/kernel, compile
#                           rngprobe to WORK/rngprobe from a copy of its
#                           sources whose kcpu.c is SRC's, and stage it alone
#                           in WORK/test.krffs, the private image rng-proof
#   rng.sh test WORK        boot the kernel with the probe under the profile
#                           max and judge its records with casecheck, as the
#                           case guest.default.rngprobe of
#                           tools/etude/schemas/case.guest.example.json,
#                           placed qemu; exit with casecheck's status, or 1
#                           when a report that passes lacks the trusted
#                           branch decided by the processor's features
set -eu
repo=$(pwd)
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
casecheck=build/tools/etude/casecheck

usage()
{
    echo 'rng: usage: rng.sh build SRC WORK | test WORK' >&2
    exit 2
}

[ "$#" -ge 2 ] || usage
case $1 in
    build)
        [ "$#" -eq 3 ] || usage
        src=$2 work=$3
        sh tests/proofs/guest.sh make rach2 build/tests/system/harness \
            build/krffs/mkfs.krffs build/krffs/stageimage.krffs \
            build/krffs/readfile.krffs "$casecheck"
        sh tests/proofs/guest.sh copy "$work" rach2/kernel
        copy=$work/probe
        mkdir -p "$copy/rach2/sys/lib" "$copy/rach2/sys/arch/x86" "$copy/tests" \
            "$copy/utils" "$copy/tools/etude/include/etude"
        cp "$src/rach2/sys/arch/x86/kcpu.c" "$copy/rach2/sys/arch/x86/"
        cp rach2/sys/arch/x86/kcpu.h "$copy/rach2/sys/arch/x86/"
        cp rach2/sys/lib/kint.h "$copy/rach2/sys/lib/"
        cp tests/rngprobe.c tests/probe.h tests/probe_report.h "$copy/tests/"
        cp tools/etude/include/etude/report.h "$copy/tools/etude/include/etude/"
        cp utils/pingcall.h "$copy/utils/"
        # shellcheck disable=SC2086 # ARCH_LDFLAGS is empty or one flag
        "${CROSS_CC:-${CROSS}gcc}" "$MFLAG" -nostdlib -static -ffreestanding \
            -Wl,-Ttext="$ARCH_USER_TEXT" -Wl,--build-id=none $ARCH_LDFLAGS \
            -o "$work/rngprobe" "$copy/tests/rngprobe.c"
        printf 'file 0755 /rngprobe %s\n' "$work/rngprobe" >"$work/rng-proof.manifest"
        sh krffs/mkimage.sh "$work/rng-proof.manifest" "$work/test.krffs" 1
        build/krffs/readfile.krffs "$work/test.krffs" /rngprobe "$work/staged"
        cmp "$work/rngprobe" "$work/staged"
        rm -f "$work/staged"
        ;;
    test)
        [ "$#" -eq 2 ] || usage
        work=$2
        cpu=$("$casecheck" processor max)
        sh tests/proofs/guest.sh boot "$work" --append 'rach2.nonet rach2.noaudio' \
            --net hubport,id=net0,hubid=0 --qemu-arg=-cpu --qemu-arg="$cpu" \
            --timeout 30 --ok '@etude 1 end ' \
            "expect=Type 'help'" 'send=exec /rngprobe' 'expect=@etude 1 end ' || :
        tr -d '\r' <"$work/console.log" | grep -a '^@etude 1 ' >"$work/records" || :
        status=0
        "$casecheck" report tools/etude/schemas/case.guest.example.json \
            guest.default.rngprobe "$RACH2_ARCH" qemu "$work/records" \
            >"$work/judged" 2>&1 || status=$?
        cat "$work/judged"
        if [ "$status" -eq 0 ]; then
            for line in 'branch source trusted decided by feature' 'outcome pass'; do
                if ! grep -Fq -- "$line" "$work/judged"; then
                    echo "rng: fail: the report passed without $line"
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
