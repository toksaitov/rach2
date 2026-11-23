#!/bin/sh
# Read the host's graphics and KVM counts at the boundary of a measured scene.
# `stop` ends the VM's profiler that `before` started, for a caller's cleanup
# when the scene ends before its `after`.
set -eu

[ "$#" = 2 ] || { echo 'usage: profile.sh DIRECTORY before|after|stop' >&2; exit 2; }
case $2 in before|after|stop) ;; *) exit 2 ;; esac

# vm ARGS: run sh with ARGS as root in the profiling VM, script on stdin.
vm()
{
    ssh -F /dev/null -p "$BED_VM_PROFILE_PORT" -i "${BED_VM_PROFILE_KEY:?}" \
        -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=yes \
        -o "UserKnownHostsFile=${BED_VM_PROFILE_HOSTS:?}" gfx@127.0.0.1 \
        sudo sh -s -- "$@"
}

if [ "$2" = stop ]; then
    [ -z "${BED_VM_PROFILE_PORT:-}" ] ||
        echo 'systemctl stop vgpu-profile-perf vgpu-profile-gpu 2> /dev/null || :' | vm
    exit 0
fi
out=$1/$2
mkdir -p "$out"
date +%s.%N > "$out/start"
cat /proc/stat > "$out/proc-stat"
debug=/sys/kernel/debug
# cpu: one line a thread: its ID, its process's, its user and system ticks
# and its name, which /proc/PID/task/TID/stat gives.
cpu()
{
    for task in /proc/[0-9]*/task/[0-9]*; do
        pid=${task%/task/*}
        stat=$(cat "$task/stat" 2> /dev/null) || continue
        comm=${stat#*\(}
        comm=${comm%\)*}
        # shellcheck disable=SC2086 # the fields after the name are words
        set -- ${stat##*\) }
        echo "${task##*/} ${pid#/proc/} ${12} ${13} $comm"
    done
}
# BED_PROFILE_THREAD names the one thread bkl/calls counts from before on:
# a name, an ID, or `busiest NAME`, the thread of that name that used the
# most processor time over the two seconds before samples, a process's first
# thread among them, which before/chosen records.
if [ "$2" = before ] && [ -n "${BED_PROFILE_THREAD:-}" ]; then
    thread=$BED_PROFILE_THREAD
    case $thread in
        'busiest '*)
            cpu > "$out/cpu-0"
            sleep 2
            cpu > "$out/cpu-1"
            thread=$(awk -v name="${thread#busiest }" '
                NR == FNR { t[$1] = $3 + $4; next }
                {
                    comm = $0
                    sub(/^[^ ]+ [^ ]+ [^ ]+ [^ ]+ /, "", comm)
                }
                ($1 in t) && comm == name {
                    d = $3 + $4 - t[$1]
                    if (best == "" || d > most) { best = $1; most = d }
                }
                END { print best }' "$out/cpu-0" "$out/cpu-1")
            [ -n "$thread" ] || { echo "profile: no thread ${BED_PROFILE_THREAD#busiest }" >&2; exit 1; }
            echo "$thread" > "$out/chosen"
            ;;
    esac
    echo "thread $thread" > "$debug/bkl/calls"
fi
for path in kvm/exit_stats bkl/calls; do
    [ ! -r "$debug/$path" ] || cat "$debug/$path" > "$out/${path##*/}"
done
for card in "$debug"/dri/[0-9]*; do
    path=$card/virtio_gpu_profile
    [ ! -r "$path" ] || cat "$path" > "$out/drm-${card##*/}"
    path=$card/virtio_gpu_events
    if [ -w "$path" ]; then
        case $2 in
            before) echo clear > "$path" ;;
            after) cat "$path" > "$out/events-${card##*/}" ;;
        esac
    fi
done
if [ "$2" = after ]; then
    # One line a thread: its ID, its process's and its name.
    for task in /proc/[0-9]*/task/[0-9]*; do
        pid=${task%/task/*}
        read -r comm 2> /dev/null < "$task/comm" && echo "${task##*/} ${pid#/proc/} $comm"
    done > "$out/threads" || :
    cpu > "$out/cpu" || :
fi
if command -v ivshmemd > /dev/null; then
    ivshmemd -i > "$out/region" 2> "$out/region.err" || :
fi
if [ -n "${BED_VM_PROFILE_PORT:-}" ]; then
    vm "$2" > "$out/vm" <<'VM'
set -eu
pid=$(pidof -s vgpu-backend)
file=/run/vgpu-profile
tr '\000' '\n' < "/proc/$pid/cmdline" | grep -Fxq -- "$file"
[ ! -e "$file" ] || unlink "$file"
kill -USR1 "$pid"
tries=0
until [ -s "$file" ]; do
    tries=$((tries + 1))
    [ "$tries" -lt 50 ] || { echo 'profile: no vm snapshot' >&2; exit 1; }
    sleep .1
done
cat "$file"
printf 'backend stat '
cat "/proc/$pid/stat"
grep '^drm-' "/proc/$pid"/fdinfo/* || :
# threads: one line a backend thread: its ID, user and system ticks and
# name; a function, since set replaces its arguments alone.
threads()
{
    for task in /proc/"$pid"/task/[0-9]*; do
        stat=$(cat "$task/stat" 2> /dev/null) || continue
        comm=${stat#*\(}
        comm=${comm%\)*}
        # shellcheck disable=SC2086 # the fields after the name are words
        set -- ${stat##*\) }
        echo "backend thread ${task##*/} ${12} ${13} $comm"
    done
}
threads
if command -v perf > /dev/null; then
    case $1 in
        before)
            # i915's busy, wait and semaphore time of the render ring, in
            # ns, over the whole VM; i915 samples a ring without execlists.
            rm -f /run/vgpu-gpu.stat
            systemd-run -q --unit=vgpu-profile-gpu --property=KillSignal=SIGINT \
                --property=RuntimeMaxSec=900s perf stat -a -x, -o /run/vgpu-gpu.stat \
                -e i915/rcs0-busy/,i915/rcs0-wait/,i915/rcs0-sema/ || :
            systemd-run -q --unit=vgpu-profile-perf --property=KillSignal=SIGINT \
                --property=RuntimeMaxSec=900s perf record -e cpu-clock -F 67 \
                --call-graph dwarf,4096 -p "$pid" -o /run/vgpu-perf.data
            ;;
        after)
            systemctl stop vgpu-profile-gpu 2> /dev/null || :
            echo 'gpu busy'
            cat /run/vgpu-gpu.stat 2> /dev/null || echo 'gpu busy unavailable'
            systemctl stop vgpu-profile-perf
            perf report --stdio --percent-limit 0.5 -i /run/vgpu-perf.data
            ;;
    esac
else
    echo 'vm perf unavailable'
fi
VM
fi
date +%s.%N > "$out/end"
