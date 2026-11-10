#!/bin/sh
# hang.sh OUT: the hang part of 78-6c's trial on the Lenovo's Haswell, as
# root, on Rach2's i915 and on Debian's Linux alike: RC6's residency over 10
# idle seconds; the nine piglit tests that hung the HD 4400 under Linux
# (tests/graphics/ref/lenovo/piglit/rerun), by the reference's commands, one
# after another, while an ordinary user's Crocus draws and reads back beside
# them; and the residency again.
#
# The ordinary user is uid and gid 1000. Its one other group is the render
# node's when that group is not root's, as Debian, whose node is 0660 and of
# group render, grants the GPU to a user; on Rach2, whose node is 0666 and of
# group 0, it has none. It need not be in passwd.
# Its programs are three piglit tests that pass under Linux, run in turn on
# /dev/dri/renderD128 until the last trigger has ended.
#
# OUT gets `summary.txt`, the lines this prints; `triggers.txt`, a line a
# trigger: its number, piglit's result, the exit status, the seconds it took
# and the command; `bystander.txt`, a line a run: the time, its number, the
# test, piglit's result and the exit status; the output of each trigger and
# of each bystander run that did not pass; `kernel.txt`, the kernel's lines
# since the start, gathered after each trigger by their times, since Rach2's
# log is a ring of 16 KiB; the engines' debugfs file before and after; and
# the error state. It prints
#
#   hang: rc6: before: N ms of residency in 10 s
#   hang: triggers: 9 run, F failed with status 1
#   hang: bystander: R runs, P passed
#   hang: kernel: H hangs, E resets of rcs0, C of the chip, W wedged
#   hang: rc6: after: N ms of residency in 10 s
#
# and sets no time limit: a trigger that never ends shows as a triggers.txt
# that stops growing, which whoever runs the step watches. The status is 0
# when every part ran, whatever it found.
set -eu

[ $# -eq 1 ] || { echo "usage: hang.sh OUT" >&2; exit 2; }
out=$1
bin=${PIGLIT_BIN:-/usr/lib/x86_64-linux-gnu/piglit/bin}
rc6=${RC6_FILE:-/sys/class/drm/card0/power/rc6_residency_ms}
dbg=${DEBUGFS_DIR:-/sys/kernel/debug/dri/0}
node=/dev/dri/renderD128

[ "$(id -u)" = 0 ] || { echo "hang: not root" >&2; exit 1; }
for t in read-front arb_copy_image-formats fbo-readpixels fbo-clear-formats gl-1.0-readpixsanity; do
    [ -x "$bin/$t" ] || { echo "hang: no $bin/$t" >&2; exit 1; }
done
[ -r "$rc6" ] || { echo "hang: no $rc6" >&2; exit 1; }
command -v python3 > /dev/null || { echo "hang: no python3" >&2; exit 1; }

mkdir -p "$out"
stop=$out/stop
home=
by=
cleanup() {
    : > "$stop"
    [ -z "$by" ] || wait "$by" 2> /dev/null || true
    rm -f "$stop" "$out/bystander.last"
    [ -z "$home" ] || rm -rf "$home"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
rm -f "$stop" "$out"/*.txt "$out"/*.out
home=$(mktemp -d /tmp/t2650-user.XXXXXX)
chown 1000:1000 "$home"
chmod 700 "$home"

say() { echo "hang: $*" | tee -a "$out/summary.txt"; }

# The render node's group, given to the ordinary user unless it is root's.
ngid=$(stat -c %g "$node")
[ "$ngid" != 0 ] || ngid=

# asuser COMMAND...: COMMAND as uid and gid 1000 with the render node's group.
asuser() {
    HOME=$home NGID=$ngid python3 -I -c 'import os, sys
os.setgroups([int(os.environ["NGID"])] if os.environ["NGID"] else [])
os.setgid(1000)
os.setuid(1000)
os.execvp(sys.argv[1], sys.argv[1:])' "$@"
}

# result FILE: piglit's last result line in FILE, or `none`.
result() {
    r=$(sed -n 's/^PIGLIT: *{"result": *"\([a-z]*\)".*/\1/p' "$1" | tail -n 1)
    echo "${r:-none}"
}

# rc6_read NAME: the residency gained over 10 seconds.
rc6_read() {
    a=$(cat "$rc6")
    sleep 10
    b=$(cat "$rc6")
    say "rc6: $1: $((b - a)) ms of residency in 10 s"
}

bystander() {
    n=0
    while [ ! -e "$stop" ]; do
        for t in fbo-readpixels fbo-clear-formats gl-1.0-readpixsanity; do
            [ ! -e "$stop" ] || break
            n=$((n + 1))
            st=0
            WAFFLE_GBM_DEVICE=$node asuser "$bin/$t" -auto -fbo > "$out/bystander.last" 2>&1 || st=$?
            r=$(result "$out/bystander.last")
            echo "$(date +%s) $n $t $r $st" >> "$out/bystander.txt"
            if [ "$st" != 0 ] || [ "$r" != pass ]; then
                cp "$out/bystander.last" "$out/bystander-$n.out"
            fi
        done
    done
}

# ktime: the time of the kernel line on standard input, or nothing.
ktime() {
    sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p'
}

klast=0
# kernel_new: append to kernel.txt the kernel's lines newer than the last
# one seen.
kernel_new() {
    dmesg | awk -v t0="$klast" '{
        t = $0; sub(/^\[ */, "", t); sub(/\].*/, "", t)
        if (t + 0 > t0 + 0) print
    }' >> "$out/kernel.txt"
    k=$(tail -n 1 "$out/kernel.txt" | ktime)
    [ -z "$k" ] || klast=$k
}

ntrig=0
# trigger COMMAND...: one of the reference's commands, as root.
trigger() {
    ntrig=$((ntrig + 1))
    s=$(date +%s)
    st=0
    "$@" > "$out/trigger-$ntrig.out" 2>&1 || st=$?
    e=$(date +%s)
    echo "$ntrig $(result "$out/trigger-$ntrig.out") $st $((e - s)) $*" >> "$out/triggers.txt"
    kernel_new
}

# count PATTERN: the lines of kernel.txt that match.
count() {
    grep -cE "$1" "$out/kernel.txt" || true
}

export PIGLIT_PLATFORM=gbm
{
    uname -a
    id
    ls -ln /dev/dri || true
    asuser id || true
    asuser sh -c "[ -r $node ] && [ -w $node ] && echo 'user: the render node opens'" || true
    if command -v eglinfo > /dev/null; then
        asuser eglinfo -B -p surfaceless 2>&1 | grep -E 'renderer|^OpenGL core profile version' || true
    fi
} > "$out/facts.txt" 2>&1
cat "$dbg/i915_engine_info" > "$out/engines-before.txt" 2> /dev/null || true
k=$(dmesg | tail -n 1 | ktime)
[ -z "$k" ] || klast=$k
: > "$out/kernel.txt"

rc6_read before

: > "$out/bystander.txt"
bystander &
by=$!
sleep 5
for first in clear-front-first ""; do
    for s in 2 4 6 8; do
        # shellcheck disable=SC2086
        trigger "$bin/read-front" $first -samples=$s -auto
    done
done
trigger "$bin/arb_copy_image-formats" --samples=2 -auto -fbo
: > "$stop"
wait "$by" || true
by=

kernel_new
cat "$dbg/i915_engine_info" > "$out/engines-after.txt" 2> /dev/null || true
head -n 200 /sys/class/drm/card0/error > "$out/error.txt" 2> /dev/null || true

say "triggers: $ntrig run, $(grep -c '^[0-9]* fail 1 ' "$out/triggers.txt" || true) failed with status 1"
say "bystander: $(grep -c . "$out/bystander.txt" || true) runs," \
    "$(grep -c ' pass 0$' "$out/bystander.txt" || true) passed"
say "kernel: $(count 'GPU HANG') hangs," \
    "$(count '(Resetting|reset:) rcs0 for ') resets of rcs0," \
    "$(count 'Resetting chip for |reset: the chip for ') of the chip," \
    "$(count '[Ww]edged') wedged"

sleep 5
rc6_read after
