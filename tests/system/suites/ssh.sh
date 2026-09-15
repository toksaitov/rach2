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
# a hang has to fail, not sit there.
#
# The card is a parameter because this machine has more than one driver and
# the capstone is worth running on each: the TCP path is the deepest thing the
# network stack does, and a driver that loses one frame in a handshake shows
# here and nowhere else. It defaults to the RTL8139, so an invocation that
# names no card runs exactly what it always ran.
#
# Usage: ssh_test.sh <kernel> <disk> <root password> <test password>
#                    [port] [card]
#

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

KERNEL="$1"
DISK="$2"
PASSWORD="$3"
TESTPASSWORD="$4"
PORT="${5:-2222}"
CARD="${6:-rtl8139}"

if [ -z "$KERNEL" ] || [ -z "$DISK" ] || [ -z "$PASSWORD" ] || [ -z "$TESTPASSWORD" ]; then
    echo "usage: ssh_test.sh <kernel> <disk> <root password> <test password> [port] [card]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
WORK="$DISK.work"
LOG="$DISK.work.log"
FEED="$DISK.work.feed"
KNOWN="$DISK.work.known"
rm -f "$WORK" "$LOG" "$FEED" "$KNOWN"

QPID=""
FPID=""
FLOOD_PIDS=""
FLOOD_DIR="$DISK.work.flood"

# Everything this run started, ended. It is written to be safe to call twice,
# because the ordinary paths call it and the trap calls it again: a script that
# only killed its machine when it succeeded or failed its own checks leaves
# QEMU running for a cancelled build, still holding the forwarded port, and the
# names that would have said so have just been removed.
#
# Each pid is taken and forgotten BEFORE it is signalled, so a trap that fires
# between the kill and the wait finds nothing left to kill: a pid this script no
# longer owns is one the host may since have given to somebody else.
cleanup_boot() {
    qpid="$QPID"
    fpid="$FPID"
    QPID=""
    FPID=""
    if [ -n "$qpid" ]; then
        kill -9 "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    if [ -n "$fpid" ]; then
        kill -9 "$fpid" 2>/dev/null || true
        wait "$fpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

# Each connection has two direct children, with a FIFO between them. Killing
# a pipeline's shell would leave its sleep running for the full five minutes.
flood_start() {
    mkdir -p "$FLOOD_DIR"
    flood_i=0
    while [ "$flood_i" -lt 40 ]; do
        flood_fifo="$FLOOD_DIR/$flood_i"
        mkfifo "$flood_fifo"
        (exec sleep 300 > "$flood_fifo") &
        FLOOD_PIDS="$FLOOD_PIDS $!"
        (exec nc localhost "$PORT" < "$flood_fifo" >/dev/null 2>&1) &
        FLOOD_PIDS="$FLOOD_PIDS $!"
        flood_i=$((flood_i + 1))
    done
}

# Reap both children of every connection, including after partial setup.
flood_stop() {
    flood_pids=$FLOOD_PIDS
    FLOOD_PIDS=""
    for flood_pid in $flood_pids; do kill -KILL "$flood_pid" 2>/dev/null || :; done
    for flood_pid in $flood_pids; do wait "$flood_pid" 2>/dev/null || :; done
    rm -rf "$FLOOD_DIR"
}

cleanup() {
    flood_stop
    cleanup_boot
    rm -f "$WORK" "$LOG" "$FEED" "$KNOWN"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
    fi
    cleanup_boot
    echo "ssh-test: FAIL: $1" >&2
    echo "ssh-test: the failing boot's console log: $DISK.fail.log" >&2
    exit 1
}

# The console is driven through tests/system/drive.sh: a line is typed when
# the output it answers has arrived, and each wait is for fresh output under
# a bound.
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"
type_line() { drive_type "$1"; }
wait_token() { drive_expect "$1" "$(scaled "$2")"; }


rm -f "$WORK"
cp "$DISK" "$WORK"

# -R makes dropbear mint its host key on first use, which needs the trusted
# entropy `-cpu max` provides; -g bars root's password from the SSH exchange, so
# root arrives by key and the test account by password, which is the policy the
# image is built around.
rm -f "$LOG" "$FEED"
mkfifo "$FEED"
qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
    -append "rach2.fsdisk" \
    -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
    -netdev "user,id=n0,hostfwd=tcp::$PORT-:22" -device "$CARD,netdev=n0" \
    -serial stdio < "$FEED" > "$LOG" 2>&1 &
QPID=$!
# The machine is started first and the feed second, which is the order that lets
# the feed know the machine's pid: opening a fifo for reading waits for a
# writer, so QEMU sits in that open until the feed arrives, and neither half
# misses a line. The feed then holds the console open only while the machine is
# up, so nothing is left typing at a machine that is gone.
DRIVE_LOG=$LOG
DRIVE_FIFO=$FEED
drive_feed "$QPID"
FPID=$DRIVE_FPID

if ! drive_login root "$PASSWORD" "$(scaled 90)"; then
    fail "the machine never reached a root prompt at its console"
fi
type_line 'dropbearmulti dropbear -R -g -p 22'
# Dropbear's one line on success, printed after the shell's prompt has come
# back, is what says it is listening; the port answering is what the key
# login below tries for its own bound.
if ! wait_token "Running in background" 60; then
    fail "dropbear did not say it was running after being started"
fi

# Root, by key. The options are written out, not held in a variable, so
# nothing here depends on a shell splitting one, and each of the three that
# narrow the attempt is there for a reason: `-i` alone only ADDS an identity,
# so an agent or a default key in the runner's own ~/.ssh would still be
# offered, and a run that passed on one of those would say nothing about the
# key this image was built around. `IdentitiesOnly` cuts the offer down to the
# file named here, and the two authentication options cut the METHOD down to
# the one being claimed.
#
# `-tt` and the `test -t` in front of the command are what make this a pty
# session, not a command run down a pipe. ssh asks for no terminal when
# it is given a command, so without them dropbear's pty path is never touched
# and the capstone would pass on a server that could not allocate one.
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
      -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
      -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
      -i "$REPO/krffs/rach2_demo_ed25519" -tt -p "$PORT" \
      root@localhost 'test -t 0 && test -t 1 && id && echo KEY""LOGIN' 2>&1 || true)
case "$OUT" in
    *KEYLOGIN*) ;;
    *) fail "the key login as root did not run its command: $OUT" ;;
esac
case "$OUT" in
    *"uid=0(root)"*) ;;
    *) fail "the key login did not arrive as root: $OUT" ;;
esac

# One password login, run through the pty driver because ssh reads a
# password from a terminal and not from a pipe. What comes back is the
# session's output plus two lines this script reads: whether a password was
# ever ASKED for, since a server that let somebody in without asking would
# otherwise be read as a password login, and the driver's own exit status,
# so a run that died before it ever spoke to the server cannot be read as an
# answer from the server.
#
# The number of prompts is an argument: a login that is expected to work gets
# the ordinary three, and one that is expected to be refused gets one, so a
# wrong password is answered once and the attempt ends instead of sitting
# through two more.
#
# The driver sizes the pty at 40 rows by 100 columns; the session's window
# size is part of what the checks below prove. The password travels in the
# environment, never in an argument a process listing could show.
ssh_password() {
    RACH2_PASSWORD="$2" "$REPO/build/tests/system/ptydrive" \
        --rows 40 --cols 100 --timeout 60 -- \
        ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 20)" -o PubkeyAuthentication=no \
        -o PreferredAuthentications=password \
        -o "NumberOfPasswordPrompts=$3" -tt \
        -p "$PORT" "$1@localhost" "$4" 2>&1
    echo "DRIVERRC=$?"
}

# Require an attempt to have been REFUSED, not merely to have run
# nothing. An ssh that could not connect, a server that had died, and a
# driver error all produce no session and no token; only the server saying
# no produces
# this line. $1 is the output, $2 names the attempt.
require_denied() {
    case "$1" in
        *"Permission denied"*) ;;
        *) fail "$2 did not end in a refusal from the server, so it proves nothing: $1" ;;
    esac
    case "$1" in
        *TIMEDOUT*) fail "$2 timed out instead of being refused: $1" ;;
    esac
}

# And require the attempt to have been made at all: the driver returns
# non-zero for a run that never reached the server.
require_ran() {
    case "$1" in
        *"DRIVERRC=0"*) ;;
        *) fail "$2 did not run to the end: $1" ;;
    esac
}

# The unprivileged account, by password, which is the half that reaches
# /etc/shadow.
OUT=$(ssh_password test "$TESTPASSWORD" 3 \
      'test -t 0 && test -t 1 && id && echo PASS""LOGIN')
require_ran "$OUT" "the password login as the test account"
case "$OUT" in
    *PASSLOGIN*) ;;
    *) fail "the password login as the test account did not run its command: $OUT" ;;
esac
case "$OUT" in
    *"uid=1000(test)"*) ;;
    *) fail "the password login did not arrive as the test account: $OUT" ;;
esac
case "$OUT" in
    *"PROMPTED=1"*) ;;
    *) fail "the test account got in without ever being asked for a password: $OUT" ;;
esac

# The window size, and the termios under it. A login server takes the size the
# client asked for and sets it on the master before the shell starts; the shell
# and everything it runs read it off the slave, which is a different descriptor
# and a different path through the kernel. `stty` is those two ioctls and
# nothing else, so it is what proves the path: `size` is TIOCGWINSZ, and `-a`
# is TCGETS as well.
OUT=$(ssh_password test "$TESTPASSWORD" 3 'stty size; stty -a')
require_ran "$OUT" "the session that reads its terminal"
case "$OUT" in
    *"40 100"*) ;;
    *) fail "stty size did not report the window the client asked for: $OUT" ;;
esac
case "$OUT" in
    *"rows 40; columns 100"*) ;;
    *) fail "stty -a did not report the window the client asked for: $OUT" ;;
esac
case "$OUT" in
    *"icanon"*) ;;
    *) fail "stty -a did not report the termios of the session's terminal: $OUT" ;;
esac
echo "ssh-test: the session's terminal reports the window and the termios: OK"

# What must be refused. Each of these is asserted twice: that no session ran,
# and that the server SAID no. The absence of a token is not a refusal, since a
# connection that was reset, a server that had died and an ssh that could not
# start all produce the same nothing; `Permission denied` is ssh reporting an
# answer it received. They also run after the two attempts that must work, which
# is a second reason the same way: the server was answering a moment ago.
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
      -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o PubkeyAuthentication=no \
      -o PasswordAuthentication=no -p "$PORT" \
      test@localhost 'echo NOAUTH""RAN' 2>&1 || true)
case "$OUT" in
    *NOAUTHRAN*) fail "a login offering no credentials at all was let in: $OUT" ;;
esac
require_denied "$OUT" "the login offering no credentials"

# The one that proves the hash is really compared, and not merely present: the
# same account, one character short of its password.
OUT=$(ssh_password test "${TESTPASSWORD}x" 1 'echo BADPASS""RAN')
require_ran "$OUT" "the wrong-password attempt"
case "$OUT" in
    *BADPASSRAN*) fail "a wrong password was accepted: $OUT" ;;
esac
require_denied "$OUT" "the wrong-password attempt"
case "$OUT" in
    *"PROMPTED=1"*) ;;
    *) fail "the wrong-password attempt was never asked for one, so its refusal proves nothing: $OUT" ;;
esac

# And root by password, with the RIGHT password, which the server was started
# with -g to refuse. Nothing is required of the prompt here: a server that does
# not offer the method has nothing to ask, and the refusal is the point.
OUT=$(ssh_password root "$PASSWORD" 1 'echo ROOTPW""RAN')
require_ran "$OUT" "root's password attempt"
case "$OUT" in
    *ROOTPWRAN*) fail "root was let in by password, which -g must refuse: $OUT" ;;
esac
require_denied "$OUT" "root's password attempt"

# A flood: forty connections opened to the server and held without a word,
# more than the fifteen the listener's backlog holds while dropbear takes
# them one at a time, while root ends the console session and logs in
# again there. The console needs no socket and root's processes are not
# counted, so the login must come back to a shell that answers `id` with
# uid 0; what the backlog cannot hold is refused with a reset, and the rest
# are dropbear's to refuse.
flood_start
# An interval, not a wait for output: the forty connections reach the guest
# and are counted only once they have been accepted or refused there. The
# interval is scaled, since a guest sharing its host with a pool of machines
# gets through less of the flood in the same wall clock.
sleep "$(scaled 5)"
# The flood is real before it is relied on: the guest's own count of
# established connections, typed at the console session about to end. It is
# counted up to four times and the highest is the reading. A host with more
# machines on it than processors can only hold the guest back from taking a
# connection, never make it take one it did not; a single count at a moment
# the guest had not caught up failed a set on 2026-09-13 at four.
established=0
attempt=0
while [ "$attempt" -lt 4 ]; do
    attempt=$((attempt + 1))
    type_line 'netstat -tn 2>/dev/null | grep -c ESTABLISHED; echo FLOOD""COUNT'
    if ! wait_token "FLOODCOUNT" 30; then
        fail "the console did not answer while the flood was on"
    fi
    seen=$(tr -d '\r' < "$LOG" | grep -B1 'FLOODCOUNT' | tail -2 | head -1 | tr -dc '0-9')
    [ "${seen:-0}" -gt "$established" ] && established=${seen:-0}
    [ "$established" -ge 5 ] && break
    sleep "$(scaled 2)"
done
if [ "$established" -lt 5 ]; then
    fail "the flood held $established connections, fewer than the five a flood must reach"
fi
type_line 'exit'
if ! drive_login root "$PASSWORD" "$(scaled 40)"; then
    fail "the console did not come back to a login prompt under the flood"
fi
type_line 'id; echo FLOOD""LOGIN'
if ! wait_token "FLOODLOGIN" 40 || ! grep -q 'uid=0' "$LOG"; then
    fail "root could not log in on the console under a connection flood"
fi
flood_stop
echo "ssh-test: root logs in on the console under a flood of forty connections: OK"

cleanup_boot

echo "ssh-test: root logged in by key and the test account by password, over a dropbear pty: OK"
echo "ssh-test: PASS"
