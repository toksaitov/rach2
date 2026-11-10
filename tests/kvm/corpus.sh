#!/bin/sh
#
# corpus.sh: run kvm-unit-tests or Linux's KVM selftests on Rach2 as a case
# list names them, and judge each result against the list.
#
# Usage: corpus.sh unit LIST
#        corpus.sh selftests LIST
#
# LIST is unit-tests.list or selftests.list of this directory: a test a line,
# its name and EXPECT, `pass`, `skip`, `fail` or `excluded`, or FLAG?A:B for a
# test whose result is the processor's: A where the `flags` of /proc/cpuinfo
# have FLAG, or where FLAG is `kvm_cap_vm_tsc_control` and /dev/kvm answers
# KVM_CAP_VM_TSC_CONTROL with a value above 0, which perl asks it, and B
# otherwise. Each test whose EXPECT is not `excluded` runs, in the order of
# the list:
#
# * `unit`: kvm-unit-tests' run_tests.sh, from a copy of /opt/kvm-unit-tests,
#   with ACCEL `kvm,kernel-irqchip=split`, as in the Linux reference,
#   and TIMEOUT 0, so that no test is judged by
#   the clock. A test passes on its PASS line and skips on its SKIP line.
# * `selftests`: each program of /opt/kvm-selftests, run in a directory of
#   its own; exit 0 is a pass and 4, kselftest's code, a skip.
#
# The copy and the directories are under TMPDIR, /tmp by default, and are
# removed on every exit. Each test's line is `kvm-corpus: NAME: RESULT`, with
# `as expected` or `expected EXPECT` after it, and each excluded one's
# `kvm-corpus: NAME: excluded: REASON`. The last line is `kvm-corpus: result:
# pass: N as expected, M excluded` or `kvm-corpus: result: fail: K of N:
# NAME...`, and the exit status 0 on a pass, 1 on a failure and 2 on a usage
# error. A program's own output goes to standard error.

usage()
{
    echo "usage: corpus.sh unit|selftests LIST" >&2
    exit 2
}
[ $# -eq 2 ] && [ -f "$2" ] || usage
kind=$1 list=$2
case $kind in
    unit) tree=/opt/kvm-unit-tests ;;
    selftests) tree=/opt/kvm-selftests ;;
    *) usage ;;
esac
[ -d "$tree" ] || { echo "corpus: no $tree" >&2; exit 1; }
case $list in
    /*) ;;
    *) list=$PWD/$list ;;
esac

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/kvm-corpus.XXXXXX") || exit 1

# The list with each EXPECT that names a flag settled by this processor's.
flags=$(awk -F: '$1 ~ /^flags[ \t]*$/ { print $2; exit }' /proc/cpuinfo 2> /dev/null)
# KVM_CHECK_EXTENSION is _IO(0xAE, 3); KVM_CAP_VM_TSC_CONTROL is 214. Perl's
# ioctl gives "0 but true" for 0 and undef for an error, both 0 as numbers.
if perl -e 'open(my $f, "<", "/dev/kvm") or exit 1;
        exit !(ioctl($f, 0xAE03, 214) + 0)' 2> /dev/null; then
    flags="$flags kvm_cap_vm_tsc_control"
fi
awk -v flags=" $flags " '
    !/^#/ && NF >= 2 {
        q = index($2, "?")
        if (q) {
            rest = substr($2, q + 1)
            c = index(rest, ":")
            has = index(flags, " " substr($2, 1, q - 1) " ")
            $2 = has ? substr(rest, 1, c - 1) : substr(rest, c + 1)
        }
        print
    }' "$list" > "$work/list"

# The tests that run, in the list's order, with what each must give.
awk '$2 != "excluded" { print $1, $2 }' "$work/list" > "$work/run"
awk '$2 == "excluded" {
    r = $0; sub(/^[^ ]+ [^ ]+ [^ ]+ ?/, "", r); print "kvm-corpus: " $1 ": excluded: " r }' "$work/list"
excluded=$(awk '$2 == "excluded"' "$work/list" | wc -l | tr -d ' ')

# PMU expectations follow the machine that runs the corpus. AMD has no
# legacy PMU presence bit, so its unit test fails when the host has none;
# the selftests read enable_pmu and skip. The Intel selftests skip on AMD.
pmu_enabled=
if [ -r /sys/module/kvm/parameters/enable_pmu ]; then
    read -r pmu_enabled < /sys/module/kvm/parameters/enable_pmu
fi
pmu_intel=0
if grep -q 'GenuineIntel' /proc/cpuinfo; then
    pmu_intel=1
fi

# results: write NAME RESULT a line to $work/got, RESULT pass, skip or fail.
: > "$work/got"
if [ "$kind" = unit ]; then
    cp -R "$tree" "$work/kut" || exit 1
    # shellcheck disable=SC2046 # one word a test
    (cd "$work/kut" && ACCEL=kvm,kernel-irqchip=split TIMEOUT=0 \
        ./run_tests.sh -a $(awk '{ print $1 }' "$work/run")) > "$work/out" 2>&1
    # The runner colours its result words; the escapes go before the match.
    sed "s/$(printf '\033')\\[[0-9;]*m//g" "$work/out" | tee "$work/plain" >&2
    awk '$1 == "PASS" { print $2, "pass" } $1 == "SKIP" { print $2, "skip" }
         $1 == "FAIL" { print $2, "fail" }' "$work/plain" > "$work/got"
else
    while read -r name _; do
        mkdir "$work/t"
        # The programs do not name themselves, so their output is told
        # apart by this line.
        echo "kvm-corpus: $name: run" >&2
        (cd "$work/t" && "$tree/$name" < /dev/null) >&2 2>&1
        case $? in
            0) echo "$name pass" ;;
            4) echo "$name skip" ;;
            *) echo "$name fail" ;;
        esac >> "$work/got"
        rm -rf "$work/t"
    done < "$work/run"
fi

# Judge each test that ran against the list.
total=0 bad=0 names=''
while read -r name expect; do
    case "$pmu_enabled:$pmu_intel:$name" in
        N:0:pmu) expect=fail ;;
        N:1:pmu) expect=skip ;;
        N:*:x86_64/pmu_counters_test|N:*:x86_64/pmu_event_filter_test|N:*:x86_64/vmx_pmu_caps_test)
            expect=skip ;;
        Y:1:x86_64/pmu_counters_test|Y:1:x86_64/vmx_pmu_caps_test)
            expect=pass ;;
    esac
    got=$(awk -v n="$name" '$1 == n { print $2; exit }' "$work/got")
    got=${got:-missing}
    total=$((total + 1))
    if [ "$got" = "$expect" ]; then
        echo "kvm-corpus: $name: $got: as expected"
    else
        echo "kvm-corpus: $name: $got: expected $expect"
        bad=$((bad + 1))
        names="$names $name"
    fi
done < "$work/run"
if [ "$bad" -eq 0 ]; then
    echo "kvm-corpus: result: pass: $total as expected, $excluded excluded"
    exit 0
fi
echo "kvm-corpus: result: fail: $bad of $total:$names"
exit 1
