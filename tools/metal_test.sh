#!/usr/bin/env sh

#
# metal_test.sh: the metal image, reached over SSH with nobody at its console.
#
# This is the rehearsal for the machine across the room. `ssh_test.sh` proves
# the SSH capstone works, but it gets there by typing at the console: log in,
# start dropbear, then connect. On the Lenovo nobody is going to do that, so
# the whole question here is different and narrower: does a machine that was
# only ever given power end up answering SSH?
#
# So this script types NOTHING at the console. It boots the metal image with a
# LAN-style address on the kernel command line, waits, and connects. Everything
# it proves follows from the machine having booted on its own: init-metal
# supervising dropbear, the address configuration reaching the stack, root's
# key authenticating, a pty, and a file crossing in each direction intact.
#
# It also kills the SSH server once and requires the machine to answer again,
# because a supervisor that starts a daemon and does not restart it is a
# supervisor that has not been tested.
#
# Every wait is bounded and the image is a copy, so a hang fails rather than
# sits, and a run leaves nothing behind.
#
# Usage: metal_test.sh <kernel> <image> <port>
#

set -e

KERNEL="$1"
IMAGE="$2"
PORT="${3:-2223}"

if [ -z "$KERNEL" ] || [ -z "$IMAGE" ]; then
    echo "usage: metal_test.sh <kernel> <image> [port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KEY="$REPO/krffs/rach2_demo_ed25519"
WORK="$IMAGE.metal.work"
LOG="$IMAGE.metal.log"
KNOWN="$IMAGE.metal.known"
SEND="$IMAGE.metal.send"
BACK="$IMAGE.metal.back"
rm -f "$WORK" "$LOG" "$KNOWN" "$SEND" "$BACK"

QPID=""

# Everything this run started, ended, and safe to call twice: the ordinary
# paths call it and the trap calls it again. The pid is taken and forgotten
# before it is signalled, so a trap firing in between finds nothing left to
# kill; a pid this script no longer owns may since have been given to somebody
# else.
cleanup_boot() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

cleanup() {
    cleanup_boot
    rm -f "$WORK" "$LOG" "$KNOWN" "$SEND" "$BACK"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$IMAGE.metal.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "metal-test: FAIL: $1" >&2
    echo "metal-test: the failing boot's console log: $IMAGE.metal.fail.log" >&2
    exit 1
}

wait_token() {
    i=0
    while [ "$i" -lt "$2" ]; do
        if grep -q "$1" "$LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

if [ ! -f "$KEY" ]; then
    echo "metal-test: no demo key at $KEY; run 'sh krffs/gen_demo_key.sh'" >&2
    exit 2
fi
cp "$IMAGE" "$WORK"

# The address is given on the kernel command line, as it will be on the stick,
# and it is a LAN-style one rather than SLIRP's: what is under test includes
# the stack running on an address nothing compiled in. SLIRP is told to serve
# that same network, so the guest's configuration and the emulated network
# agree, which is exactly the arrangement the real machine will be in with a
# real router. -cpu max because dropbear's -R mints a host key and the kernel
# refuses untrusted entropy for it.
GUEST_NET="192.168.90.0/24"
GUEST_IP="192.168.90.15"
GUEST_GW="192.168.90.2"
qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
    -initrd "$WORK" \
    -append "rach2.ip=$GUEST_IP/24 rach2.gateway=$GUEST_GW rach2.netconsole" \
    -netdev "user,id=n0,net=$GUEST_NET,host=$GUEST_GW,hostfwd=tcp::$PORT-$GUEST_IP:22" \
    -device rtl8139,netdev=n0 \
    -serial stdio < /dev/null > "$LOG" 2>&1 &
QPID=$!

if ! wait_token "rach2 login" 90; then
    fail "the machine never reached its login prompt"
fi
if ! grep -q "IPv4 $GUEST_IP/24" "$LOG"; then
    fail "the machine did not take the address from its command line"
fi

# Nothing has been typed and nothing will be. The SSH server is up because init
# started it, or it is not up at all.
ssh_run() {
    ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes \
        root@127.0.0.1 "$1" 2>>"$LOG"
}

i=0
while [ "$i" -lt 60 ]; do
    if ssh_run 'echo METAL_READY' 2>/dev/null | grep -q METAL_READY; then
        break
    fi
    sleep 2
    i=$((i + 2))
done
if [ "$i" -ge 60 ]; then
    fail "no SSH answer: init did not bring an unattended server up"
fi
echo "metal-test: the machine answered SSH with nobody at its console: OK"

# A pty, which is what an interactive session needs and what a command run
# without one never exercises.
if ! ssh -i "$KEY" -p "$PORT" -tt \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes \
        root@127.0.0.1 'tty; exit' 2>>"$LOG" | grep -q '/dev/pts/'; then
    fail "the session did not get a pty"
fi
echo "metal-test: the session runs on a pty: OK"

# A file each way, compared by hash rather than by eye. The content is a
# thousand lines so a truncation or a lost chunk cannot pass unnoticed.
i=0
while [ "$i" -lt 1000 ]; do
    echo "line $i the quick brown fox jumps over the lazy dog" >> "$SEND"
    i=$((i + 1))
done
SENT_SUM="$(cksum < "$SEND" | awk '{print $1, $2}')"

if ! ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes \
        root@127.0.0.1 'cat > /tmp/metal.in' < "$SEND" 2>>"$LOG"; then
    fail "the file could not be sent"
fi
GUEST_SUM="$(ssh_run 'cksum < /tmp/metal.in' | awk '{print $1, $2}')"
if [ "$GUEST_SUM" != "$SENT_SUM" ]; then
    fail "the file arrived changed: sent [$SENT_SUM], the machine read [$GUEST_SUM]"
fi
echo "metal-test: a file sent over SSH arrived byte for byte: OK"

if ! ssh_run 'cat /tmp/metal.in' > "$BACK" 2>>"$LOG"; then
    fail "the file could not be read back"
fi
BACK_SUM="$(cksum < "$BACK" | awk '{print $1, $2}')"
if [ "$BACK_SUM" != "$SENT_SUM" ]; then
    fail "the file came back changed: sent [$SENT_SUM], received [$BACK_SUM]"
fi
echo "metal-test: the same file came back byte for byte: OK"

# The supervision itself. Killing the server is the one thing that separates a
# supervisor from a script that ran a command once at boot, and the server's pid
# is read before and after: a kill that silently failed would leave the original
# process answering, and the test would pass having proved nothing.
BEFORE="$(ssh_run 'pidof dropbearmulti' | tr -d '\r' | awk '{print $NF}')"
if [ -z "$BEFORE" ]; then
    fail "could not find the SSH server's pid on the machine, so killing it would prove nothing"
fi
ssh_run "kill -9 $BEFORE" >/dev/null 2>&1 || true
sleep 2
i=0
AFTER=""
while [ "$i" -lt 60 ]; do
    AFTER="$(ssh_run 'pidof dropbearmulti' 2>/dev/null | tr -d '\r' | awk '{print $NF}')"
    if [ -n "$AFTER" ] && ssh_run 'echo METAL_AGAIN' 2>/dev/null | grep -q METAL_AGAIN; then
        break
    fi
    AFTER=""
    sleep 2
    i=$((i + 2))
done
if [ -z "$AFTER" ]; then
    fail "the SSH server was killed and init did not bring it back"
fi
if [ "$AFTER" = "$BEFORE" ]; then
    fail "the server answering is the one that was supposedly killed (pid $BEFORE), so nothing was proved"
fi
echo "metal-test: the server was killed (pid $BEFORE) and init brought back another (pid $AFTER): OK"

# And the console login is still there, which is the recovery path for a
# machine whose network is the broken part.
if ! grep -q "rach2 login" "$LOG"; then
    fail "the console login is not on the console"
fi

cleanup_boot
echo "metal-test: PASS"
