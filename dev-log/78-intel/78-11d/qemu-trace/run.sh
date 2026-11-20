#!/bin/sh
# Record the immediate WebGPU picture in a disposable QEMU overlay.
set -eu

tree=$(git rev-parse --show-toplevel)
cd "$tree"
step=dev-log/78-intel/78-11d/qemu-trace
disk=${RACH2_TRACE_DISK:-"$tree/build/runs/t3504t/qemu.disk"}
port=${RACH2_TRACE_PORT:-26516}
out="$tree/build/runs/t3504-wake-trace-$(date '+%Y%m%dT%H%M%S')"
root=/data/trial/t3504t
stage=$root/qemu-trace
test -f "$disk"
test -f build/x86_64/rach2/kernel
mkdir -p build/tmp build/runs
mkdir "$out"
export TMPDIR="$tree/build/tmp"
qemu=
trap '[ -z "$qemu" ] || { kill "$qemu" 2>/dev/null || :; wait "$qemu" 2>/dev/null || :; }' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

guest()
{
    ssh -i "$tree/krffs/rach2_demo_ed25519" -p "$port" -o BatchMode=yes \
        -o ConnectTimeout=4 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 \
        -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o LogLevel=ERROR \
        root@127.0.0.1 "$@"
}

git rev-parse HEAD > "$out/commit"
shasum -a 256 build/x86_64/rach2/kernel > "$out/kernel.sha256"
qemu-system-x86_64 -accel tcg,thread=multi -cpu max -smp 4 -m 6G -machine q35 \
    -kernel build/x86_64/rach2/kernel \
    -append 'rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial/t3504t/root-5ed936d79361e924.img rach2.slot=trialroot' \
    -snapshot -drive "file=$disk,format=raw,if=none,id=d0" -device ide-hd,drive=d0,bus=ide.0 \
    -nic "user,model=rtl8139,hostfwd=tcp:127.0.0.1:$port-:22" \
    -display none -vga none -monitor none -no-reboot -serial "file:$out/serial.log" &
qemu=$!
printf 'qemu: evidence: %s\n' "$out"
old=0
quiet=0
until guest true </dev/null 2>/dev/null; do
    kill -0 "$qemu"
    size=$(wc -c < "$out/serial.log")
    if [ "$size" -ne "$old" ]; then
        tail -n 1 "$out/serial.log"
        old=$size quiet=0
    else
        quiet=$((quiet + 2))
    fi
    [ "$quiet" -lt 60 ] || { tail -n 25 "$out/serial.log" >&2; exit 1; }
    sleep 2
done
guest 'cat /proc/cmdline; cat /proc/mounts; id desktop' > "$out/boot.txt"
grep '^/dev/loop0 / ext2 ' "$out/boot.txt"
tar -czf "$out/payload.tar.gz" tests/graphics/browser "$step/picture.py"
guest "mkdir '$stage' && tar -xzf - -C '$stage' && mkdir '$stage/out' && chown desktop:desktop '$stage/out'" \
    < "$out/payload.tar.gz"

status=0
guest "cd '$stage' && sh -s '$step/picture.py'" <<'GUEST' || status=$?
set -eu
trace=/sys/kernel/debug/bkl/trace
test -e "$trace" || mount -t debugfs debugfs /sys/kernel/debug
printf 'start\n' > "$trace"
trap 'printf "stop\n" > "$trace"' EXIT
status=0
timeout -k 10 240 su -s /bin/sh desktop -c "PYTHONDONTWRITEBYTECODE=1 python3 -u '$1'" || status=$?
printf 'stop\n' > "$trace"
cat "$trace" > kernel.bin
printf 'clear\n' > "$trace"
exit "$status"
GUEST
guest "tar -czf - -C '$stage' out kernel.bin" > "$out/guest.tar.gz"
tar -xzf "$out/guest.tar.gz" -C "$out"
guest 'sync; poweroff -f' </dev/null || :
exit "$status"
