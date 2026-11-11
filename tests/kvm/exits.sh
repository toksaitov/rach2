#!/bin/sh
#
# exits.sh: count a guest's exits by the processor's exit code, on Rach2 or
# on Linux, around one run of a command, such as kvm-boot.sh's boot of the
# Debian guest.
#
# Usage: exits.sh COMMAND [ARGUMENT...]
#
# As root. On Rach2 the counts are those of /sys/kernel/debug/kvm/exit_stats,
# cleared before COMMAND, with the cycles each code waited for the big lock
# and spent in the kernel; on Linux, those of a histogram trigger on the
# kvm:kvm_exit trace event keyed by its exit reason, set before COMMAND and
# removed after, with the totals of /sys/kernel/debug/kvm. Either way every
# guest of the machine is counted, so nothing else may run one meanwhile.
# The output, after COMMAND's own, is
#
#   kvm-exits: kernel: rach2|linux RELEASE
#   kvm-exits: code CODE exits N [kernel K user U wait_cycles W
#       handle_cycles H] NAME
#   kvm-exits: total: N exits in S s
#
# a line for each code with exits, CODE in hexadecimal and NAME Linux's for
# it, which Rach2's file gives and Linux's histogram does not; the
# bracketed fields are Rach2's alone. S is COMMAND's time on the host's
# clock, for the record. The status is COMMAND's, or 2 when neither source
# of counts is there.

if [ $# -lt 1 ]; then
    echo "usage: exits.sh COMMAND [ARGUMENT...]" >&2
    exit 2
fi
debug=/sys/kernel/debug
stats=$debug/kvm/exit_stats
trace=
for t in /sys/kernel/tracing $debug/tracing; do
    if [ -e "$t/events/kvm/kvm_exit/trigger" ]; then
        trace=$t
        break
    fi
done
trigger='hist:keys=exit_reason:vals=hitcount:sort=exit_reason:size=256'

# On a signal the trigger is removed by the exit trap.
armed=
trap '[ -z "$armed" ] || echo "!$trigger" > "$trace/events/kvm/kvm_exit/trigger"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [ -e "$stats" ]; then
    kind=rach2
    echo 0 > "$stats" || exit 2
elif [ -n "$trace" ]; then
    kind=linux
    echo "$trigger" > "$trace/events/kvm/kvm_exit/trigger" || exit 2
    armed=1
    before=$(cat "$debug/kvm/exits" 2>/dev/null || echo 0)
else
    echo "kvm-exits: neither $stats nor the kvm:kvm_exit trace event is there" >&2
    exit 2
fi

start=$(date +%s)
"$@"
status=$?
end=$(date +%s)

echo "kvm-exits: kernel: $kind $(uname -r)"
if [ "$kind" = rach2 ]; then
    awk -v secs=$((end - start)) '
        NR > 2 {
            name = $7
            for (i = 8; i <= NF; i++)
                name = name " " $i
            printf "kvm-exits: code %s exits %s kernel %s user %s wait_cycles %s handle_cycles %s %s\n",
                   $1, $2, $3, $4, $5, $6, name
            total += $2
        }
        NR == 1 { print "kvm-exits: " $0 }
        END { printf "kvm-exits: total: %d exits in %d s\n", total, secs }' "$stats"
else
    # Lines of the form "{ exit_reason: 30 } hitcount: 1234".
    sed -n 's/^{ exit_reason: *\([0-9]*\) *} *hitcount: *\([0-9]*\).*/\1 \2/p' \
        "$trace/events/kvm/kvm_exit/hist" |
        awk -v secs=$((end - start)) '
            { printf "kvm-exits: code 0x%x exits %s\n", $1, $2; total += $2 }
            END { printf "kvm-exits: total: %d exits in %d s\n", total, secs }'
    after=$(cat "$debug/kvm/exits" 2>/dev/null || echo 0)
    echo "kvm-exits: debugfs: exits $((after - before))"
fi
exit $status
