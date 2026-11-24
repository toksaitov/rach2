#!/bin/sh
# Print every thread's state, wait channel, system call and times, and the
# kernel's lock and interrupt counters, as root, for a stalled prefix.
echo "DUMP BEGIN $(cat /proc/uptime)"
cat /proc/loadavg
for t in /proc/[0-9]*/task/[0-9]*; do
    pid=${t#/proc/}; pid=${pid%%/*}; tid=${t##*/}
    comm=$(cat $t/comm 2>/dev/null)
    st=$(sed -n 's/^State:\t//p' $t/status 2>/dev/null)
    sw=$(sed -n 's/^\(.*_ctxt_switches\):\t/\1=/p' $t/status 2>/dev/null | tr '\n' ' ')
    wchan=$(cat $t/wchan 2>/dev/null)
    sc=$(cat $t/syscall 2>/dev/null)
    stat=$(cut -d' ' -f14,15 $t/stat 2>/dev/null)
    echo "T $pid $tid $comm [$st] wchan=$wchan syscall=$sc utime,stime=$stat $sw"
done
[ -d /sys/kernel/debug/bkl ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
for f in /sys/kernel/debug/bkl/calls /sys/kernel/debug/bkl/long /sys/kernel/debug/bkl/tick \
         /sys/kernel/debug/bkl/profile /sys/kernel/debug/sched/debug /proc/interrupts \
         /proc/lockdep_stats /proc/fs/lockwait /proc/stat; do
    echo "F $f"
    cat $f 2>&1 | head -200
done
echo "DUMP END $(cat /proc/uptime)"
