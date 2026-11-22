#!/bin/sh
# Read the host's graphics and KVM counts at the boundary of a measured scene.
set -eu

[ "$#" = 2 ] || { echo 'usage: profile.sh DIRECTORY before|after' >&2; exit 2; }
case $2 in before|after) ;; *) exit 2 ;; esac
out=$1/$2
mkdir -p "$out"
date +%s.%N > "$out/start"
cat /proc/stat > "$out/proc-stat"
debug=/sys/kernel/debug
for path in kvm/exit_stats bkl/calls; do
    [ ! -r "$debug/$path" ] || cat "$debug/$path" > "$out/${path##*/}"
done
for card in "$debug"/dri/[0-9]*; do
    path=$card/virtio_gpu_profile
    [ ! -r "$path" ] || cat "$path" > "$out/drm-${card##*/}"
done
if command -v ivshmemd > /dev/null; then
    ivshmemd -i > "$out/region" 2> "$out/region.err" || :
fi
if [ -n "${BED_VM_PROFILE_PORT:-}" ]; then
    ssh -F /dev/null -p "$BED_VM_PROFILE_PORT" -i "${BED_VM_PROFILE_KEY:?}" \
        -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=yes \
        -o "UserKnownHostsFile=${BED_VM_PROFILE_HOSTS:?}" gfx@127.0.0.1 \
        sudo sh -s -- "$2" > "$out/vm" <<'VM'
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
if command -v perf > /dev/null; then
    case $1 in
        before)
            systemd-run -q --unit=vgpu-profile-perf --property=KillSignal=SIGINT \
                --property=RuntimeMaxSec=900s perf record -e cpu-clock -F 67 \
                --call-graph dwarf,4096 -p "$pid" -o /run/vgpu-perf.data
            ;;
        after)
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
