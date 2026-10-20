#!/bin/sh
# xsave.sh: the test of tests/proofs/xsave.json, xsaveprobe's report judged
# as etude will judge it, beside the two signal-frame host tests.
#
# Usage: guest.sh exec WORK sh tests/proofs/xsave.sh max|qemu64 WORK
#
# Both boot WORK/kernel with WORK/test.krffs, run /xsaveprobe from the debug
# shell, and judge its `@etude 1` records with tools/etude/casecheck as the
# case guest.default.xsaveprobe of tools/etude/schemas/case.guest.example.json,
# placed qemu, for the architecture ARCH names.
#
# max boots under the processor profile max, which declares XSAVE and AVX,
# and then compiles and runs fpuframe_hosttest.c and fpuframe64_hosttest.c as
# tests/run host does. It prints `xsave: report: pass` or `xsave: report:
# fail` (casecheck's status 1) and `xsave: host tests: pass` or `xsave: host
# tests: fail`, and exits 0 when both pass.
#
# qemu64 boots under the profile qemu64, which has neither feature. The probe
# then passes by its branches fxsave and none, which the judge must refuse
# for the profile max, as it refuses any processor that lacks a feature its
# profile declares, and accept for a processor without XSAVE and AVX.
set -eu
[ "$#" -eq 2 ] || { echo 'xsave: usage: xsave.sh max|qemu64 WORK' >&2; exit 2; }
profile=$1
work=$2
casecheck=build/tools/etude/casecheck
manifest=tools/etude/schemas/case.guest.example.json
case=guest.default.xsaveprobe

# boot: boot the kernel under $profile, run xsaveprobe and keep its records
# in $work/$profile.records.
boot()
{
    status=0
    build/tests/system/harness --kernel "$work/kernel" --initrd "$work/test.krffs" \
        --qemu-arg=-cpu --qemu-arg="$("$casecheck" processor "$profile")" \
        --append rach2.rescue --timeout 60 --steptmo 30 --ok '@etude 1 end ' \
        --log "$work/$profile.serial.log" \
        "expect=Type 'help'" 'send=exec /xsaveprobe' 'expect=@etude 1 end ' \
        >"$work/$profile.log" 2>&1 || status=$?
    grep -a 'xsaveprobe\|XSAVEPROBE\|HARNESS' "$work/$profile.serial.log" "$work/$profile.log" || :
    echo "xsave: boot: $profile: status $status"
    tr -d '\r' <"$work/$profile.serial.log" | grep -a '^@etude 1 ' >"$work/$profile.records" || :
}

# host_tests: compile and run the two frame host tests; return 0 when both
# compile and pass.
host_tests()
{
    failed=0
    for t in fpuframe_hosttest fpuframe64_hosttest; do
        rm -f "$work/$t"
        ${CC:-cc} -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
            -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
            -Wno-empty-body -ffreestanding -I rach2/sys -I rach2/sys/arch/x86 \
            -I rach2/sys/arch/i686 -I rach2/sys/lib -I rach2/sys/proc \
            -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -o "$work/$t" \
            "tests/host/$t.c" rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c || {
            echo "xsave: $t did not compile"
            failed=1
            continue
        }
        "$work/$t" || failed=1
    done
    return "$failed"
}

case $profile in
    max)
        boot
        report=0
        "$casecheck" report "$manifest" "$case" "$ARCH" qemu "$work/max.records" || report=$?
        case $report in
            0) echo 'xsave: report: pass' ;;
            1) echo 'xsave: report: fail' ;;
            *) echo "xsave: report: not judged, casecheck's status $report" ;;
        esac
        host=0
        host_tests || host=1
        if [ "$host" -eq 0 ]; then
            echo 'xsave: host tests: pass'
        else
            echo 'xsave: host tests: fail'
        fi
        [ "$report" -eq 0 ] && [ "$host" -eq 0 ]
        ;;
    qemu64)
        boot
        judged=0
        "$casecheck" report "$manifest" "$case" "$ARCH" qemu "$work/qemu64.records" \
            >"$work/qemu64.judged" 2>&1 || judged=$?
        cat "$work/qemu64.judged"
        if [ "$judged" -ne 1 ] ||
            ! grep -Fq 'the program took none where the processor gives avx' "$work/qemu64.judged"; then
            echo 'xsave: fail: the report passed without AVX under the profile max'
            exit 1
        fi
        "$casecheck" report -f xsave=absent -f avx=absent "$manifest" "$case" "$ARCH" native \
            "$work/qemu64.records" || {
            echo 'xsave: fail: the report fails where the processor has neither'
            exit 1
        }
        echo 'xsave: qemu64: the profile max refuses the report, and a processor without XSAVE and AVX passes it'
        ;;
    *)
        echo 'xsave: usage: xsave.sh max|qemu64 WORK' >&2
        exit 2
        ;;
esac
