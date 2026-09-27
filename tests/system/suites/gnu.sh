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
# the next, so a run names every row the machine cannot yet do.
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

cleanup() {
    qpid=$QPID
    QPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    rm -f "$DISK" "$KNOWN" "$DISK.back"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    echo "gnu-test: FAIL: $1" >&2
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

# ssh_as USER KEY COMMAND: run COMMAND on the machine as USER, by KEY.
ssh_as() {
    ssh -i "$2" -p "$PORT" -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 30)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey "$1@127.0.0.1" "$3"
}

# The server makes its host keys before it listens, which on an emulated
# machine takes a minute or more; the bound is on the time since the first
# try, whatever each try took.
start=$("$RUNUTIL" now)
until ssh_as root "$ROOT_KEY" 'echo GNU""READY' 2>/dev/null | grep -q GNUREADY; do
    [ $(($("$RUNUTIL" now) - start)) -lt "$(scaled 900)" ] ||
        fail "no SSH answer: init did not bring OpenSSH's server up"
    # interval: a try of SSH every two seconds until the server answers
    sleep 2
done
echo "gnu-test: OpenSSH's server answered with nobody at the console: OK"

OUT=$(ssh_as root "$ROOT_KEY" 'id -u; ls /etc/ssh' 2>&1)
case "$OUT" in
    0*ssh_host_ed25519_key*) ;;
    *) fail "root by key did not arrive as root on a machine with its own host keys: $OUT" ;;
esac
echo "gnu-test: root logs in by key, and the machine made its host keys: OK"
OUT=$(ssh_as test "$TEST_KEY" 'id -u' 2>&1)
[ "$OUT" = 1000 ] || fail "the test account by key did not arrive as uid 1000: $OUT"
echo "gnu-test: the test account logs in by key: OK"

# ssh_password USER PASSWORD PROMPTS COMMAND: a password login through the
# pty driver, since ssh reads a password from a terminal; the driver's
# status and whether a password was asked for are in what it prints.
ssh_password() {
    RACH2_PASSWORD="$2" "$PTYDRIVE" --rows 40 --cols 100 --timeout "$(scaled 120)" -- \
        ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 30)" -o PubkeyAuthentication=no \
        -o PreferredAuthentications=password \
        -o "NumberOfPasswordPrompts=$3" -tt -p "$PORT" "$1@127.0.0.1" "$4" 2>&1
    echo "DRIVERRC=$?"
}
OUT=$(ssh_password test "$TESTPASSWORD" 3 'id -u; echo PASS""LOGIN')
case "$OUT" in
    *DRIVERRC=0*PROMPTED=1*|*PROMPTED=1*DRIVERRC=0*) ;;
    *) fail "the test account's password login did not run to the end with one prompt: $OUT" ;;
esac
case "$OUT" in
    *1000*PASSLOGIN*) ;;
    *) fail "the test account's password login did not arrive as uid 1000: $OUT" ;;
esac
echo "gnu-test: the test account logs in by password, checked against /etc/shadow: OK"
OUT=$(ssh_password root "$PASSWORD" 1 'echo ROOT""PASSWORD')
case "$OUT" in
    *ROOTPASSWORD*) fail "root logged in by password, which PermitRootLogin must refuse: $OUT" ;;
    *"Permission denied"*) ;;
    *) fail "root's password attempt did not end in a refusal from the server: $OUT" ;;
esac
echo "gnu-test: root's password is refused: OK"

# The rows' script, sent by scp and fetched back by sftp, whose server is
# OpenSSH's sftp-server; scp speaks the same protocol since OpenSSH 9.
ssh_as root "$ROOT_KEY" 'mkdir -p /tmp/gnu' || fail "the rows' directory on the machine"
scp -P "$PORT" -i "$ROOT_KEY" -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o BatchMode=yes "$REPO/tests/system/gnu_rows.sh" root@127.0.0.1:/tmp/gnu/rows.sh \
    > /dev/null 2>&1 || fail "scp could not put the rows' script on the machine"
printf 'get /tmp/gnu/rows.sh %s\n' "$DISK.back" |
    sftp -P "$PORT" -i "$ROOT_KEY" -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
    -o BatchMode=yes -b - root@127.0.0.1 > /dev/null 2>&1 ||
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
    if grep -q "^GNUROW OK $name\$" "$DISK.$name.log"; then
        echo "gnu-test: $name: OK"
        continue
    fi
    FAILED="$FAILED $name"
    if [ "$status" -eq 124 ]; then
        echo "gnu-test: $name: FAIL: no answer within its bound" >&2
    else
        echo "gnu-test: $name: FAIL: $(grep '^GNUROW FAIL' "$DISK.$name.log" || tail -n 3 "$DISK.$name.log")" >&2
    fi
done
[ -z "$FAILED" ] || fail "$(echo "$FAILED" | wc -w | tr -d ' ') of $TOTAL rows:$FAILED"
echo "gnu-test: PASS"
