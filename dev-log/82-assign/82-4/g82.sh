#!/bin/sh
# g82.sh: the guest's side of 82-4b's step on the Lenovo, which g82.service
# runs as root when the guest's command line has g82=MODE. Every line goes to
# the serial console, which gpu.sh keeps and prints.
#
#   g82=probe  the assigned GPU's identity, i915's lines in the log and the
#              display's state, then power off
#   g82=run    the same, then Weston on the GPU with step 9's weston.ini and
#              glmark2 2023.01's default set fullscreen, GL then GLES, as
#              step 11 of the second discovery run ran them, with the GPU's
#              clock read each second; then power off
#   g82=hold   the same Weston, then glmark2's terrain scene for ever, with
#              "G82 LOAD N" each second it runs, for gpu.sh -K to kill QEMU
#              under load
#
# It ends with "g82: result: pass: ..." or "g82: result: fail: REASON", and
# powers the guest off but in hold mode while glmark2 runs.

exec > /dev/ttyS0 2>&1
mode=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^g82=//p' | tail -n 1)
user=gfx
uid=$(id -u $user)
run=/run/user/$uid
card=/dev/dri/card0
freq=/sys/class/drm/card0/gt_act_freq_mhz

say() { echo "g82: $*"; }
off() { systemctl --no-block poweroff; exit 0; }
fail() { say "result: fail: $*"; off; }
as_user() { runuser -u $user -- env XDG_RUNTIME_DIR=$run WAYLAND_DISPLAY=wayland-1 "$@"; }

say "start: $mode: kernel $(uname -r)"
case $mode in probe | run | hold) ;; *) fail "no mode $mode" ;; esac
d=/sys/bus/pci/devices/0000:00:02.0
[ -d $d ] || fail 'no function at 00:02.0'
say "gpu: $(cat $d/vendor):$(cat $d/device) class $(cat $d/class) driver $(basename "$(readlink $d/driver)")"
n=0
while [ ! -e $card ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
dmesg | grep -i -E 'i915|drm|dmar|vt-d|fbcon' | sed 's/^/g82: log: /'
[ -e $card ] || fail "no $card after 60 s"
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
head -n 40 /sys/kernel/debug/dri/0/i915_display_info | sed 's/^/g82: display: /'
for c in /sys/class/drm/card0-*; do say "connector: ${c##*/} $(cat $c/status)"; done
b=/sys/class/backlight/intel_backlight
[ -d $b ] && say "backlight: $(cat $b/brightness) of $(cat $b/max_brightness), actual $(cat $b/actual_brightness)"
[ "$mode" = probe ] && { say 'result: pass: probe'; off; }

# Weston as step 11's gweston.sh started it, through the guest's seatd.
mkdir -p $run && chown $user: $run && chmod 700 $run
systemctl start seatd
as_user env LIBSEAT_BACKEND=seatd setsid weston --backend=drm --config=/etc/g82/weston.ini \
    --socket=wayland-1 --log=$run/weston.log < /dev/null > $run/weston.out 2>&1 &
n=0
while [ ! -S $run/wayland-1 ] && [ $n -lt 30 ]; do sleep 1; n=$((n + 1)); done
grep -i -E 'renderer|GL version|output|mode|atomic|backlight' $run/weston.log | head -n 20 |
    sed 's/^/g82: weston: /'
[ -S $run/wayland-1 ] || { tail -n 20 $run/weston.log | sed 's/^/g82: weston: /'; fail 'no Weston'; }
say "weston: up after $n s"

if [ "$mode" = hold ]; then
    as_user glmark2-wayland --fullscreen --run-forever -b terrain > /tmp/g82-hold.log 2>&1 &
    g=$!
    n=0
    while kill -0 $g 2> /dev/null; do
        sleep 1
        n=$((n + 1))
        echo "G82 LOAD $n $(cat $freq) MHz"
    done
    sed 's/^/g82: hold: /' /tmp/g82-hold.log
    fail 'glmark2 ended on its own'
fi

# One glmark2 run: NAME PROGRAM; its log, its score and the GPU's clock.
bench() {
    ( while :; do cat $freq; sleep 1; done > /tmp/g82-$1.mhz ) &
    s=$!
    ( while :; do sleep 10; echo "g82: tick: $1 $(cat $freq) MHz"; done ) &
    t=$!
    t0=$(date +%s)
    as_user "$2" --fullscreen > /tmp/g82-$1.log 2>&1
    rc=$?
    kill $s $t
    sed "s/^/g82: $1: /" /tmp/g82-$1.log
    score=$(sed -n 's/.*glmark2 Score: *\([0-9][0-9]*\).*/\1/p' /tmp/g82-$1.log)
    say "score: $1: ${score:-none}, rc $rc, $(($(date +%s) - t0)) s," \
        "clock mean $(awk '{ s += $1; n++ } END { if (n) printf "%.0f", s / n }' /tmp/g82-$1.mhz) MHz"
    [ "$rc" = 0 ] && [ -n "$score" ]
}
bench gl glmark2-wayland && gl=$score || fail "glmark2-wayland: rc $rc, score ${score:-none}"
bench gles glmark2-es2-wayland && gles=$score || fail "glmark2-es2-wayland: rc $rc, score ${score:-none}"
pkill -x weston
say "result: pass: run: gl $gl gles $gles"
off
