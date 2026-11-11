#!/bin/sh
# shellcheck disable=SC2086 # no path here has a space
# locks.sh stk|0ad NAME: one replay of games.sh with the GPU's clock pinned at
# 1000 MHz, as Rach2's runs of 80-41 pin it, and calls.bt counting the game's
# system calls, ioctl requests and kernel samples from its start to its end;
# the game's user and system time from /proc/PID/stat, and /proc/stat's,
# before and after. Output in $OUT/locks/NAME; games.sh's in
# $OUT/games/NAME.
set -u
. /root/ref80/lib.sh
need bpftrace pgrep
K=$1; N=$2; O=$OUT/locks/$N
case $K in
stk) P=supertuxkart ;;
0ad) P=pyrogenesis ;;
*) echo "locks: unknown game $K" >&2; exit 2 ;;
esac
rm -rf $O; mkdir -p $O
min=$(cat $GPU/gt_min_freq_mhz)
echo 1000 > $GPU/gt_min_freq_mhz
sh $R80/games.sh $K $N > $O/games.out 2>&1 &
g=$!
i=0
until pid=$(pgrep -o -u gfx -x $P); do
	i=$((i + 1))
	[ $i -ge 600 ] && { echo "locks: no $P after 120 s" >&2; break; }
	sleep 0.2
done
if [ -n "${pid:-}" ]; then
	say "locks: $P is $pid" | tee $O/run.log
	cat /proc/stat > $O/stat-before
	cat /proc/$pid/stat > $O/pid-before
	bpftrace -q $R80/calls.bt $pid > $O/calls.txt 2> $O/bpftrace.err &
	b=$!
	while kill -0 $pid 2> /dev/null; do
		cat /proc/$pid/stat > $O/pid-last 2> /dev/null
		sleep 1
	done
	kill -INT $b
	wait $b
	cat /proc/stat > $O/stat-after
	say "locks: $P ended" | tee -a $O/run.log
fi
wait $g
echo $min > $GPU/gt_min_freq_mhz
cat $GPU/gt_min_freq_mhz > $O/min-after
