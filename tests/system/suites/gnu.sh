#!/usr/bin/env sh

#
# gnu.sh: the GNU system on its ext2 root (`tests/run gnu`).
#
# Boots the ordinary kernel with `rach2.root=ext2` on a disk whose one
# partition is a copy of build/images/os-real-gnu.ext2 (vendor/gnu/Readme.md)
# and drives it over SSH with nobody at its console, the way the machine is
# driven: init starts OpenSSH's sshd, which makes its host keys on this
# first boot. Root logs in by key, the test account by key and by password,
# and root's password is refused. scp and sftp put a file on the machine and
# take it back. Then each port does its main job on the machine, one row
# each, as tests/system/gnu_rows.sh runs it: the names the path finds, glibc
# and its locales, zlib, OpenSSL with the CA bundle, OpenSSH's clients
# against the machine's own server, ncurses, less, Vim, Perl, curl over TLS,
# Git, coreutils, bash, grep, sed, gawk, findutils, diffutils, patch, tar,
# gzip and xz; GNU make building a small project; GCC and G++ compiling and
# linking a C and a C++ program that run; GDB stopping at a breakpoint,
# stepping, printing a variable and a backtrace; the SpiderMonkey shell
# running a script in its JIT; and chromium-headless-shell printing the DOM
# of a local page. Each row has its own bound and a failed row does not stop
# the next, so a run reports every row the machine cannot yet do.
#
# Usage: gnu.sh KERNEL IMAGE DISK PORT ROOT_PASSWORD TEST_PASSWORD
#

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -u

KERNEL=$1
IMAGE=$2
DISK=$3
PORT=$4
PASSWORD=$5
TESTPASSWORD=$6
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
RUNUTIL="$REPO/build/tests/system/runutil"
DEADLINE="$REPO/build/tests/system/deadline"
PTYDRIVE="$REPO/build/tests/system/ptydrive"
MKMBR="$REPO/build/tools/image/mkmbr"
ROOT_KEY="$REPO/krffs/rach2_demo_ed25519"
TEST_KEY="$REPO/krffs/rach2_test_ed25519"
LOG="$DISK.log"
KNOWN="$DISK.known"
QPID=""

. "$REPO/tools/setup/e2fsprogs_path.sh"

# The machine, stopped; safe to call twice, since the pid is forgotten
# before it is signalled.
stop_machine() {
    qpid=$QPID
    QPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}
cleanup() {
    stop_machine
    rm -f "$DISK" "$KNOWN" "$DISK.back"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# What the SSH server said is in the machine's service log, not on its
# console, so a failure reads the log off the stopped machine's disk, from
# the partition at 1 MiB.
fail() {
    echo "gnu-test: FAIL: $1" >&2
    stop_machine
    if [ -f "$DISK" ]; then
        debugfs -R "cat /var/log/services" "$DISK?offset=1048576" > "$DISK.services" 2>/dev/null
        if [ -s "$DISK.services" ]; then
            echo "gnu-test: the machine's service log: $DISK.services" >&2
        fi
    fi
    echo "gnu-test: the console log: $LOG" >&2
    exit 1
}

for key in "$ROOT_KEY" "$TEST_KEY"; do
    [ -f "$key" ] || fail "no demo key at $key; run 'sh krffs/gen_demo_key.sh'"
done

# The disk: the volume at LBA 2048, as the machine has its root, copied
# sparse, since the image is 4 GiB with most of it free.
SIZE_MB=$(( $(wc -c < "$IMAGE") / 1048576 ))
rm -f "$DISK" "$KNOWN" "$LOG"
dd if=/dev/zero of="$DISK" bs=1048576 count=0 seek=$((SIZE_MB + 2)) 2>/dev/null ||
    fail "the disk file"
dd if="$IMAGE" of="$DISK" bs=1048576 seek=1 conv=notrunc,sparse 2>/dev/null ||
    fail "the volume on the disk"
"$MKMBR" --lba "$DISK" "2048:$((SIZE_MB * 2048)):83" > /dev/null || fail "the disk's table"

# Two GiB, for the compilers and the browser; the disk on an AHCI port,
# whose transfers are DMA, since the rows read most of the volume.
qemu-system-i386 -cpu max -m 2048 -display none -kernel "$KERNEL" \
    -append "rach2.root=ext2" \
    -device ich9-ahci,id=ahci0 \
    -drive "if=none,id=drv0,file=$DISK,format=raw" -device ide-hd,drive=drv0,bus=ahci0.0 \
    -netdev "user,id=n0,hostfwd=tcp::$PORT-:22" -device rtl8139,netdev=n0 \
    -serial stdio < /dev/null > "$LOG" 2>&1 &
QPID=$!

"$RUNUTIL" until "$LOG" "ext2: root: /dev/disk0p1" "$(scaled 120)" ||
    fail "the machine did not mount the GNU system's volume as its root"
"$RUNUTIL" until "$LOG" "rach2 login" "$(scaled 300)" ||
    fail "the machine never reached its login prompt"
"$RUNUTIL" until "$LOG" "init: sshd: started as pid" "$(scaled 60)" ||
    fail "init did not start the SSH server"

# ssh_as BOUND USER KEY COMMAND: run COMMAND on the machine as USER, by KEY,
# under the deadline tool at BOUND seconds: ConnectTimeout bounds only the
# connection and its key exchange, and a session that stalls after them
# would never return.
ssh_as() {
    "$DEADLINE" "$1" ssh -i "$3" -p "$PORT" -o StrictHostKeyChecking=no \
        -o "UserKnownHostsFile=$KNOWN" -o "ConnectTimeout=$(scaled 30)" -o BatchMode=yes \
        -o IdentitiesOnly=yes -o PreferredAuthentications=publickey "$2@127.0.0.1" "$4"
}

# The server makes its host keys before it listens, which on an emulated
# machine takes a minute or more. The bound is on the time since the first
# try, and each try is held to what is left of it.
start=$("$RUNUTIL" now)
while :; do
    left=$((start + $(scaled 900) - $("$RUNUTIL" now)))
    [ "$left" -gt 0 ] || fail "no SSH answer: init did not bring OpenSSH's server up"
    try=$(scaled 60)
    [ "$try" -le "$left" ] || try=$left
    ssh_as "$try" root "$ROOT_KEY" 'echo GNU""READY' 2>/dev/null | grep -q GNUREADY && break
    # interval: a try of SSH every two seconds until the server answers
    sleep 2
done
echo "gnu-test: OpenSSH's server answered with nobody at the console: OK"

OUT=$(ssh_as "$(scaled 120)" root "$ROOT_KEY" 'id -u; ls /etc/ssh' 2>&1)
case "$OUT" in
    0*ssh_host_ed25519_key*) ;;
    *) fail "root by key did not arrive as root on a machine with its own host keys: $OUT" ;;
esac
echo "gnu-test: root logs in by key, and the machine made its host keys: OK"
OUT=$(ssh_as "$(scaled 120)" test "$TEST_KEY" 'id -u' 2>&1)
[ "$OUT" = 1000 ] || fail "the test account by key did not arrive as uid 1000: $OUT"
echo "gnu-test: the test account logs in by key: OK"

# ssh_password USER PASSWORD PROMPTS COMMAND: a password login through the
# pty driver, since ssh reads a password from a terminal. The driver does
# not pass on the status of what it runs, and ends with 0 when its time runs
# out, printing TIMEDOUT, so ssh runs under a shell that prints ssh's own
# status as SSHRC=N once ssh has ended; the driver's status, whether a
# password was asked for and TIMEDOUT are in what the driver prints. The
# driver scales its own timeout by the run's factor, so it is given the
# idle host's seconds.
ssh_password() {
    RACH2_PASSWORD="$2" "$PTYDRIVE" --rows 40 --cols 100 --timeout 120 -- \
        sh -c 'ssh "$@"; echo "SSHRC=$?"' ssh \
        -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 30)" -o PubkeyAuthentication=no \
        -o PreferredAuthentications=password \
        -o "NumberOfPasswordPrompts=$3" -tt -p "$PORT" "$1@127.0.0.1" "$4" 2>&1
    echo "DRIVERRC=$?"
}
OUT=$(ssh_password test "$TESTPASSWORD" 3 'id -u; echo PASS""LOGIN')
case "$OUT" in
    *TIMEDOUT*) fail "the test account's password login did not end within its bound: $OUT" ;;
    *DRIVERRC=0*PROMPTED=1*|*PROMPTED=1*DRIVERRC=0*) ;;
    *) fail "the test account's password login did not run to the end with one prompt: $OUT" ;;
esac
case "$OUT" in
    *1000*PASSLOGIN*SSHRC=0*) ;;
    *) fail "the test account's password login did not arrive as uid 1000 and end with ssh's status 0: $OUT" ;;
esac
echo "gnu-test: the test account logs in by password, checked against /etc/shadow: OK"
OUT=$(ssh_password root "$PASSWORD" 1 'echo ROOT""PASSWORD')
case "$OUT" in
    *TIMEDOUT*) fail "root's password attempt did not end within its bound: $OUT" ;;
    *ROOTPASSWORD*) fail "root logged in by password, which PermitRootLogin must refuse: $OUT" ;;
    *"Permission denied"*SSHRC=255*) ;;
    *) fail "root's password attempt did not end in a refusal from the server: $OUT" ;;
esac
echo "gnu-test: root's password is refused: OK"

# The rows' script, sent by scp and fetched back by sftp, whose server is
# OpenSSH's sftp-server; scp speaks the same protocol since OpenSSH 9. Each
# is held to a bound, as every ssh is.
ssh_as "$(scaled 120)" root "$ROOT_KEY" 'mkdir -p /tmp/gnu' ||
    fail "the rows' directory on the machine"
"$DEADLINE" "$(scaled 120)" scp -P "$PORT" -i "$ROOT_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes "$REPO/tests/system/gnu_rows.sh" \
    root@127.0.0.1:/tmp/gnu/rows.sh > /dev/null 2>&1 ||
    fail "scp could not put the rows' script on the machine"
printf 'get /tmp/gnu/rows.sh %s\n' "$DISK.back" |
    "$DEADLINE" "$(scaled 120)" sftp -P "$PORT" -i "$ROOT_KEY" -o StrictHostKeyChecking=no \
    -o "UserKnownHostsFile=$KNOWN" -o BatchMode=yes -b - root@127.0.0.1 > /dev/null 2>&1 ||
    fail "sftp could not fetch the rows' script back"
cmp -s "$REPO/tests/system/gnu_rows.sh" "$DISK.back" ||
    fail "the file sftp fetched is not the one scp sent"
echo "gnu-test: scp puts a file on the machine and sftp takes it back, byte for byte: OK"

# Each row with the seconds it takes on an idle host under the emulator.
ROWS="names:120 glibc:120 zlib:120 openssl:180 openssh:600 ncurses:120 less:120
vim:120 perl:180 curl:600 git:300 coreutils:120 bash:120 grep:120 sed:120
gawk:120 findutils:120 diffutils:120 patch:120 tar:120 gzip:120 xz:120
make:600 gcc:900 gdb:600 js:600 chromium:1200"
FAILED=""
TOTAL=0
for row in $ROWS; do
    name=${row%%:*}
    TOTAL=$((TOTAL + 1))
    "$DEADLINE" "$(scaled "${row#*:}")" \
        ssh -i "$ROOT_KEY" -p "$PORT" -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 30)" -o BatchMode=yes -o IdentitiesOnly=yes \
        root@127.0.0.1 "/bin/bash /tmp/gnu/rows.sh $name" > "$DISK.$name.log" 2>&1
    status=$?
    if [ "$status" -eq 0 ] && grep -q "^GNUROW OK $name\$" "$DISK.$name.log"; then
        echo "gnu-test: $name: OK"
        continue
    fi
    FAILED="$FAILED $name"
    if [ "$status" -eq 124 ]; then
        echo "gnu-test: $name: FAIL: no answer within its bound" >&2
    elif grep -q "^GNUROW OK $name\$" "$DISK.$name.log"; then
        echo "gnu-test: $name: FAIL: the row printed its OK, but ssh ended with status $status" >&2
    else
        echo "gnu-test: $name: FAIL: $(grep '^GNUROW FAIL' "$DISK.$name.log" || tail -n 3 "$DISK.$name.log")" >&2
    fi
done
[ -z "$FAILED" ] || fail "$(echo "$FAILED" | wc -w | tr -d ' ') of $TOTAL rows:$FAILED"
echo "gnu-test: PASS"
