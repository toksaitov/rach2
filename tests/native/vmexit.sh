#!/bin/sh
# vmexit.sh: kvm-unit-tests' vmexit group on the machine's KVM, each exit's
# cost in the guest's TSC cycles set beside Linux's on the same machine, as
# the native case kvm/vmexit of rows.cases runs it on the Lenovo.
#
#   vmexit.sh LINUX
#
# Run as root in the case's workspace on the x86-64 GNU root, whose KVM layer
# has kvm-unit-tests under /opt/kvm-unit-tests (vendor/gnu/kvm). The tree is
# copied into the workspace, since its runner writes its logs where it is
# started, and `ACCEL=kvm ./run_tests.sh -g vmexit` runs the group's twelve
# tests. LINUX, tests/kvm/ref/linux-vmexit.txt, has Linux's cycles for each
# test, three rounds. A line for each test:
#
#   vmexit: NAME rach2 C linux L1 L2 L3 ratio R
#
# R being C over the mean of Linux's. The case passes when every test of the
# group passes and prints its cycles; the cycles are measured, not judged,
# since the comparison with Linux's belongs to the KVM tasks that read it.
# The last line is `vmexit: result: pass` or `fail: why`.
set -eu

[ $# -eq 1 ] || {
    echo "usage: vmexit.sh LINUX" >&2
    exit 2
}
linux=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -f "$linux" ] || {
    echo "vmexit: result: fail: no $1"
    exit 1
}
[ -c /dev/kvm ] || {
    echo "vmexit: result: fail: no /dev/kvm"
    exit 1
}
rm -rf kut
cp -a /opt/kvm-unit-tests kut
cd kut
status=0
ACCEL=kvm ./run_tests.sh -g vmexit -v > ../run.out 2>&1 || status=$?
cat ../run.out
cat logs/*.log > ../logs.txt 2> /dev/null || : > ../logs.txt
cd ..
awk -v status="$status" '
    FILENAME == ARGV[1] {
        if ($0 !~ /^#/ && NF == 4) { want[$1] = $2 " " $3 " " $4; mean[$1] = ($2 + $3 + $4) / 3; n++ }
        next
    }
    FILENAME == ARGV[2] {
        sub(/\r$/, "")
        if (NF == 2 && ($1 in want) && $2 ~ /^[0-9]+$/) got[$1] = $2
        next
    }
    # run_tests.sh colours each result word, on a terminal or not.
    { gsub(/\033\[[0-9;]*m/, "") }
    /^PASS / { pass++ }
    /^(FAIL|SKIP) / { bad = bad " " $2 }
    END {
        missing = ""
        for (name in want) {
            if (!(name in got)) { missing = missing " " name; continue }
            printf "vmexit: %s rach2 %d linux %s ratio %.2f\n", name, got[name], want[name],
                (mean[name] > 0 ? got[name] / mean[name] : 0)
        }
        if (bad != "") print "vmexit: result: fail: not passed:" bad
        else if (pass != n) print "vmexit: result: fail: " pass " of " n " tests passed, status " status
        else if (missing != "") print "vmexit: result: fail: no cycles for" missing
        else { print "vmexit: result: pass"; exit 0 }
        exit 1
    }' "$linux" logs.txt run.out
