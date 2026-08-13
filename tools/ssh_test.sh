#!/usr/bin/env sh

#
# ssh_test.sh: the SSH capstone, run without a person at the keyboard.
#
# The capstone has always been a procedure in a Readme: boot os-real, log in at
# the console, start dropbear, and ssh in from another terminal. That is fine
# for a demonstration and useless as a gate, so this is the same procedure with
# the waiting written down.
#
# It logs in BOTH ways, because they prove different things. The key login is
# root's, and it is the one the image is set up for. The password login is the
# unprivileged account's, and it is the one that reaches the credential library:
# dropbear reads /etc/shadow and compares a hash, which is the whole of what
# this system's authentication is.
#
# The disk is a copy, so a run leaves nothing behind, and every wait is bounded:
# a hang has to fail rather than sit there.
#
# Usage: ssh_test.sh <kernel> <disk> <root password> <test password> <port>
#

set -e

KERNEL="$1"
DISK="$2"
PASSWORD="$3"
TESTPASSWORD="$4"
PORT="${5:-2222}"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$PASSWORD" ] || [ -z "$TESTPASSWORD" ]; then
    echo "usage: ssh_test.sh <kernel> <disk> <root password> <test password> [port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
KNOWN="$DISK.work.known"
rm -f "$WORK" "$LOG" "$FEED" "$KNOWN"
trap 'rm -f "$WORK" "$LOG" "$FEED" "$KNOWN"' EXIT INT TERM

QPID=""
FPID=""

cleanup_boot() {
    kill -9 "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    kill "$FPID" 2>/dev/null || true
    rm -f "$FEED"
}

fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "ssh-test: FAIL: $1" >&2
    echo "ssh-test: the failing boot's console log: $DISK.fail.log" >&2
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

if ! command -v expect >/dev/null 2>&1; then
    echo "ssh-test: expect(1) is needed for the password half; install it" >&2
    exit 2
fi

rm -f "$WORK"
cp "$DISK" "$WORK"

# -R makes dropbear mint its host key on first use, which needs the trusted
# entropy `-cpu max` provides; -g bars root's password from the SSH exchange, so
# root arrives by key and the test account by password, which is the policy the
# image is built around.
rm -f "$LOG" "$FEED"
mkfifo "$FEED"
(
    sleep 14
    printf 'root\n'
    sleep 4
    printf '%s\n' "$PASSWORD"
    sleep 6
    printf 'dropbearmulti dropbear -R -g -p 22\n'
    sleep 600
) > "$FEED" &
FPID=$!
qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
    -append "rach2.fsdisk" \
    -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
    -netdev "user,id=n0,hostfwd=tcp::$PORT-:22" -device rtl8139,netdev=n0 \
    -serial stdio < "$FEED" > "$LOG" 2>&1 &
QPID=$!

if ! wait_token "rach2 login" 90; then
    fail "the machine never reached its login prompt"
fi
# Dropbear says nothing on success, so what is waited for is the console coming
# back to a prompt after the command, and then the port answering.
sleep 25

# Root, by key. The options are written out rather than held in a variable, so
# nothing here depends on a shell splitting one.
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
      -o ConnectTimeout=20 -o BatchMode=yes \
      -i "$REPO/krffs/rach2_demo_ed25519" -p "$PORT" \
      root@localhost 'id; echo KEY""LOGIN' 2>&1 || true)
case "$OUT" in
    *KEYLOGIN*) ;;
    *) fail "the key login as root did not run its command: $OUT" ;;
esac
case "$OUT" in
    *"uid=0(root)"*) ;;
    *) fail "the key login did not arrive as root: $OUT" ;;
esac

# The unprivileged account, by password, which is the half that reaches
# /etc/shadow.
OUT=$(expect -c "
set timeout 60
log_user 0
spawn ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=$KNOWN \
    -o ConnectTimeout=20 -o PubkeyAuthentication=no -p $PORT test@localhost \
    {id; echo PASS\"\"LOGIN}
expect {
  \"assword:\" { send \"$TESTPASSWORD\r\"; exp_continue }
  eof { puts \$expect_out(buffer) }
  timeout { puts TIMEDOUT }
}" 2>&1 || true)
case "$OUT" in
    *PASSLOGIN*) ;;
    *) fail "the password login as the test account did not run its command: $OUT" ;;
esac
case "$OUT" in
    *"uid=1000(test)"*) ;;
    *) fail "the password login did not arrive as the test account: $OUT" ;;
esac

cleanup_boot

echo "ssh-test: root logged in by key and the test account by password, over a dropbear pty: OK"
echo "ssh-test: PASS"
