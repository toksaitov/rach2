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
# have FLAG, and B otherwise; A and B may each be FLAG?A:B. Four FLAGs are
# not the processor's flags: `kvm_cap_vm_tsc_control`, where /dev/kvm answers
# KVM_CAP_VM_TSC_CONTROL with a value above 0, which perl asks it;
# `kvm_lbr`, where the PERF_CAPABILITIES that /dev/kvm offers has an LBR
# format, bits 5:0, which perl reads with KVM_GET_MSRS;
# `kvm_pmu`, where /sys/module/kvm/parameters/enable_pmu reads Y; and
# `intel`, where /proc/cpuinfo's vendor_id is GenuineIntel. Each test whose
# EXPECT is not `excluded` runs, in the order of the list:
#
# * `unit`: kvm-unit-tests' run_tests.sh, from a copy of /opt/kvm-unit-tests,
#   with ACCEL `kvm,kernel-irqchip=split`, as in the Linux reference,
#   and TIMEOUT 0, so that no test is judged by
#   the clock. A test passes on its PASS line and skips on its SKIP line.
# * `selftests`: each program of /opt/kvm-selftests, run in a directory of
#   its own; exit 0 is a pass and 4, kselftest's code, a skip.
#
# The copy and the directories are under TMPDIR, /tmp by default, and are
# removed on every exit; kernel.nmi_watchdog reads 0 for the run and is put
# back on every exit. Each test's line is `kvm-corpus: NAME: RESULT`, with
# `as expected` or `expected EXPECT` after it, and a kvm-unit-test's own log
# on standard error after the second, and each excluded one's
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
watchdog=
trap '[ -z "$watchdog" ] || echo "$watchdog" > /proc/sys/kernel/nmi_watchdog
    [ -z "$work" ] || rm -rf "$work"' EXIT
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
# KVM_GET_MSRS is _IOWR(0xAE, 0x88, struct kvm_msrs), 8 bytes, and reads
# feature MSRs from /dev/kvm: one entry, PERF_CAPABILITIES, 0x345.
if perl -e 'open(my $f, "<", "/dev/kvm") or exit 1;
        my $b = pack("LLLLQ", 1, 0, 0x345, 0, 0);
        exit !((ioctl($f, 0xC008AE88, $b) + 0) == 1 &&
            (unpack("LLLLQ", $b))[4] & 0x3F)' 2> /dev/null; then
    flags="$flags kvm_lbr"
fi
pmu=
[ ! -r /sys/module/kvm/parameters/enable_pmu ] ||
    read -r pmu < /sys/module/kvm/parameters/enable_pmu
[ "$pmu" != Y ] || flags="$flags kvm_pmu"
! grep -q '^vendor_id[[:space:]]*: GenuineIntel' /proc/cpuinfo 2> /dev/null ||
    flags="$flags intel"
awk -v flags=" $flags " '
    # settle(E): the result E names, where E is a result or FLAG?A:B, and
    # A and B are each the same again.
    function settle(e,    q, rest, i, d, ch) {
        q = index(e, "?")
        if (q == 0)
            return e
        rest = substr(e, q + 1)
        d = 0
        for (i = 1; i <= length(rest); i++) {
            ch = substr(rest, i, 1)
            if (ch == "?")
                d++
            else if (ch == ":" && d-- == 0)
                break
        }
        if (index(flags, " " substr(e, 1, q - 1) " "))
            return settle(substr(rest, 1, i - 1))
        return settle(substr(rest, i + 1))
    }
    !/^#/ && NF >= 2 {
        $2 = settle($2)
        print
    }' "$list" > "$work/list"

# The tests that run, in the list's order, with what each must give.
awk '$2 != "excluded" { print $1, $2 }' "$work/list" > "$work/run"
awk '$2 == "excluded" {
    r = $0; sub(/^[^ ]+ [^ ]+ [^ ]+ ?/, "", r); print "kvm-corpus: " $1 ": excluded: " r }' "$work/list"
excluded=$(awk '$2 == "excluded"' "$work/list" | wc -l | tr -d ' ')

# results: write NAME RESULT a line to $work/got, RESULT pass, skip or fail.
: > "$work/got"
# The hard-lockup detector holds a counter, which the PMU tests count on
# having: kvm-unit-tests' `pmu` and `pmu_lbr` require kernel.nmi_watchdog to
# read 0, as their configuration checks, and `pmu_counters_test` uses every
# general counter. It reads 0 for the run and its value comes back on every
# exit.
if read -r w < /proc/sys/kernel/nmi_watchdog 2> /dev/null && [ "$w" != 0 ] &&
    echo 0 2> /dev/null > /proc/sys/kernel/nmi_watchdog; then
    watchdog=$w
fi
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
    got=$(awk -v n="$name" '$1 == n { print $2; exit }' "$work/got")
    got=${got:-missing}
    total=$((total + 1))
    if [ "$got" = "$expect" ]; then
        echo "kvm-corpus: $name: $got: as expected"
    else
        echo "kvm-corpus: $name: $got: expected $expect"
        # The runner's log of the test names each subtest's result.
        log=$work/kut/logs/$name.log
        [ "$kind" != unit ] || [ ! -f "$log" ] ||
            sed "s/$(printf '\033')\\[[0-9;]*m//g" "$log" >&2
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
