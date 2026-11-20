#!/bin/sh
# threads.sh: one line for each thread, `PID TID COMM RUN WAIT SLICES`, from
# /proc/PID/task/TID/comm and schedstat (nanoseconds run and waited on a run
# queue, and slices), which Linux and Rach2 both give.
for t in /proc/[0-9]*/task/[0-9]*; do
    s=$(cat "$t/schedstat" 2> /dev/null) || continue
    c=$(tr ' ' '_' < "$t/comm" 2> /dev/null) || continue
    p=${t#/proc/}
    echo "${p%%/*} ${t##*/} $c $s"
done
