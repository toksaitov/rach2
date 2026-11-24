#!/bin/sh
# Card 5798's 2 MB/s step under Linux: the same Proton and one.py as on
# Rach2, Linux's page cache dropped before each prefix, the account's reads
# of the system disk held at RATE bytes a second by a cgroup's io.max from
# the prefix's start until it ends or CAP seconds pass.
# usage: linux.sh COUNT RATE CAP
set -u
count=$1 rate=$2 cap=$3
say() { echo "linux: $(date -u +%T) $*"; }
sudo mkdir -p /opt
sudo tar -xf /tmp/proton.tar -C /opt && rm -f /tmp/proton.tar
mkdir -p /tmp/games /tmp/.X11-unix
tar -xf /tmp/bundle.tar -C /tmp/games
root=$(findmnt -no SOURCE /)
disk=/dev/$(lsblk -no PKNAME "$root" | head -n 1)
mm=$(lsblk -dno MAJ:MIN "$disk" | tr -d ' ')
say "root $root disk $disk $mm; $(uname -r)"
cg=/sys/fs/cgroup/t5798
sudo sh -c "echo +io > /sys/fs/cgroup/cgroup.subtree_control"
sudo mkdir -p $cg
reads() { awk '{print $1, $3 * 512}' /sys/block/${disk#/dev/}/stat; }
i=0
while [ $i -lt "$count" ]; do
    i=$((i + 1))
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    rm -f /tmp/one-go
    log=/tmp/one-$i.log
    GAMES_LAUNCHER='sh /tmp/games/rach2-game.sh' \
        python3 /tmp/games/one.py /tmp/one-out-$i /tmp/one-go > $log 2>&1 &
    t=0
    until grep -q "ONE WESTON" $log; do
        sleep 1; t=$((t + 1))
        [ $t -lt 300 ] || { say "prefix $i: no Weston"; cat $log; exit 1; }
    done
    # Every process of the account, Weston's and this script's too, as
    # QEMU's throttle on Rach2 holds the whole disk.
    for p in $(ps -u "$(id -u)" -o pid=); do
        echo "$p" | sudo tee $cg/cgroup.procs > /dev/null 2>&1
    done
    r0=$(reads)
    echo "$mm rbps=$rate" | sudo tee $cg/io.max > /dev/null
    touch /tmp/one-go
    t=0 full=
    while ! grep -q "ONE END" $log; do
        sleep 1; t=$((t + 1))
        if [ -z "$full" ] && [ $t -ge "$cap" ]; then
            echo "$mm rbps=max" | sudo tee $cg/io.max > /dev/null; full=$t
            say "prefix $i: disk at full speed after $t s; reads $(reads)"
        fi
        [ $((t % 30)) -ne 0 ] || say "prefix $i: $t s, reads $(reads)"
        if [ -n "$full" ] && [ $((t - full)) -ge 120 ]; then
            say "STALL prefix $i: no ONE END 120 s after full speed"
            cat $log
            for task in /proc/[0-9]*/task/[0-9]*; do
                pid=${task#/proc/}; pid=${pid%%/*}
                [ "$(stat -c %u /proc/$pid 2>/dev/null)" = "$(id -u)" ] || continue
                echo "T $pid ${task##*/} $(cat $task/comm 2>/dev/null) $(sed -n 's/^State:\t//p' $task/status 2>/dev/null) wchan=$(cat $task/wchan 2>/dev/null) syscall=$(sudo cat $task/syscall 2>/dev/null | cut -d' ' -f1-3)"
            done
            grep -h "err:" "$(ls -td /tmp/one.*/ | head -n 1)"logs/*.log 2>/dev/null
            exit 3
        fi
    done
    echo "$mm rbps=max" | sudo tee $cg/io.max > /dev/null
    r1=$(reads)
    t=0
    until grep -q "ONE DONE" $log; do
        sleep 1; t=$((t + 1))
        [ $t -lt 120 ] || { say "STALL prefix $i: no ONE DONE"; cat $log; exit 3; }
    done
    say "prefix $i: $(grep 'ONE END' $log); reads before $r0 after $r1"
    w=$(ls -td /tmp/one.*/ | head -n 1)
    grep -h "err:" "$w"logs/*.log 2>/dev/null | sed "s/^/linux: prefix $i: /"
    wait
    rm -rf "$w"
done
say "DONE $count prefixes"
