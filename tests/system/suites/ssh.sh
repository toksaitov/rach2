#!/usr/bin/env sh

#
# ssh.sh: log in to os-real over SSH with nobody at the keyboard.
#
# The procedure is a person's: boot os-real, log in at the console, start
# dropbear, and ssh in from the host. The suite types the console lines and
# waits on what the machine prints.
#
# It logs in both ways, because they test different things. The key login is
# root's, and it is the one the image is set up for. The password login is the
# unprivileged account's, and it is the one that reaches the credential
# library: dropbear reads /etc/shadow and compares a hash, which is all of
# this system's authentication.
#
# The disk is a copy, so a run leaves nothing behind, and every wait is
# bounded, so a hang fails.
#
# The card is a parameter so the suite runs on each network driver: the TCP
# path is the deepest thing the network stack does, and a driver that loses
# one frame in a handshake fails here. It defaults to the RTL8139.
#
# Usage: ssh.sh <kernel> <disk> <root password> <test password>
#               [port] [card]
#

# The emulator of the architecture tests/system/suites.mk names.
QEMU=${RACH2_QEMU:-qemu-system-i386}

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

# cleanup_boot: stop the machine and its feeder and remove the FIFO. It is safe
# to call twice, since the ordinary paths call it and the trap calls it again;
# without the trap a cancelled build would leave QEMU running and holding the
# forwarded port, with its files removed.
#
# Each pid is cleared before it is signalled, so a trap that fires between the
# kill and the wait finds nothing left to kill: a pid this script no longer
# owns may since have been given to another process.
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

# flood_start: open forty connections to the forwarded port and hold them
# silent. Each connection is two direct children, a sleep writing a FIFO and
# an nc reading it, since killing a pipeline's shell would leave its sleep
# running for the full five minutes. The directory is made afresh: a run
# killed during the flood leaves its FIFOs, and mkfifo would fail on them.
flood_start() {
    rm -rf "$FLOOD_DIR"
    mkdir -p "$FLOOD_DIR"
    flood_i=0
    while [ "$flood_i" -lt 40 ]; do
        flood_fifo="$FLOOD_DIR/$flood_i"
        mkfifo "$flood_fifo"
        # interval: a holder keeps a flood writer open until the flood is killed
        (exec sleep 300 > "$flood_fifo") &
        FLOOD_PIDS="$FLOOD_PIDS $!"
        (exec nc localhost "$PORT" < "$flood_fifo" >/dev/null 2>&1) &
        FLOOD_PIDS="$FLOOD_PIDS $!"
        flood_i=$((flood_i + 1))
    done
}

# flood_stop: kill and reap both children of every connection, including
# after a partial start, and remove the FIFOs.
flood_stop() {
    flood_pids=$FLOOD_PIDS
    FLOOD_PIDS=""
    for flood_pid in $flood_pids; do kill -KILL "$flood_pid" 2>/dev/null || :; done
    for flood_pid in $flood_pids; do wait "$flood_pid" 2>/dev/null || :; done
    rm -rf "$FLOOD_DIR"
}

# cleanup: stop the flood and the boot and remove the working files.
cleanup() {
    flood_stop
    cleanup_boot
    rm -f "$WORK" "$LOG" "$FEED" "$KNOWN"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

# fail REASON: keep the console log as $DISK.fail.log, stop the boot, print
# REASON and exit 1.
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
# type_line LINE types LINE; wait_token TOKEN SECONDS waits for TOKEN for a
# scaled SECONDS.
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
"$QEMU" -cpu max -display none -kernel "$KERNEL" \
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
# nothing here depends on a shell splitting one. `-i` alone only adds an
# identity, so an agent or a default key in the runner's own ~/.ssh would
# still be offered, and a run that passed on one of those would say nothing
# about the key this image was built around. `IdentitiesOnly` limits the
# offer to the file named here, and the two authentication options limit the
# method to public key.
#
# `-tt` and the `test -t` in front of the command make this a pty session and
# not a command run down a pipe. ssh asks for no terminal when it is given a
# command, so without them dropbear's pty path is never used and the suite
# would pass on a server that could not allocate one.
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

# ssh_password USER PASSWORD PROMPTS COMMAND: log in as USER by PASSWORD and
# run COMMAND, through the pty driver, since ssh reads a password from a
# terminal and not from a pipe. It prints the session's output and two lines
# this script reads: whether a password was asked for (PROMPTED=1), since a
# server that let somebody in without asking would otherwise be read as a
# password login, and the driver's exit status (DRIVERRC=), so a run that
# died before it reached the server is not read as the server's answer.
#
# A login expected to work gets the ordinary three PROMPTS, and one expected
# to be refused gets one, so a wrong password is answered once and the
# attempt ends.
#
# The driver sizes the pty at 40 rows by 100 columns, which the window checks
# below read back. The password is passed in the environment, never in an
# argument a process listing could show.
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

# require_denied OUTPUT NAME: fail unless OUTPUT of the attempt NAME has the
# server's refusal and no time-out. An ssh that could not connect, a server
# that had died, and a driver error all give no session and no token; only
# the server's answer gives `Permission denied`.
require_denied() {
    case "$1" in
        *"Permission denied"*) ;;
        *) fail "$2 did not end in a refusal from the server, so it proves nothing: $1" ;;
    esac
    case "$1" in
        *TIMEDOUT*) fail "$2 timed out instead of being refused: $1" ;;
    esac
}

# require_ran OUTPUT NAME: fail unless the driver ran the attempt NAME to the
# end; it returns non-zero for a run that never reached the server.
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
# and everything it runs read it off the slave, which is a different
# descriptor and a different path through the kernel. `stty` makes those two
# ioctls and nothing else, so it tests that path: `size` is TIOCGWINSZ, and
# `-a` is TCGETS as well.
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

# What must be refused. Each of these is checked twice: that no session ran,
# and that the server refused. A missing token alone is not a refusal, since
# a connection that was reset, a server that had died and an ssh that could
# not start all print no token; `Permission denied` is ssh reporting an
# answer it received. They also run after the two attempts that must work, so
# the server was answering a moment before.
OUT=$(ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
      -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o PubkeyAuthentication=no \
      -o PasswordAuthentication=no -p "$PORT" \
      test@localhost 'echo NOAUTH""RAN' 2>&1 || true)
case "$OUT" in
    *NOAUTHRAN*) fail "a login offering no credentials at all was let in: $OUT" ;;
esac
require_denied "$OUT" "the login offering no credentials"

# The same account with one character added to its password, which tests that
# the hash is compared.
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

# Root by password, with the right password, which the server was started
# with -g to refuse. Nothing is required of the prompt here: a server that
# does not offer the method has nothing to ask.
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
sleep "$(scaled 5)" # interval: the forty connections are counted once the guest has taken them
# flood_count: ask the console for the guest's count of established
# connections and set `established` to it; fail if the console does not
# answer or netstat fails, since a failed listing gives no number and must
# not be read as zero. The count must be more than none while the forty are
# held, and none once they are released below. How many the guest holds
# depends on its backlog and the emulator's forwarding, and is printed and not
# judged.
flood_count() {
    # shellcheck disable=SC2016 # The guest's shell expands the count.
    type_line 'if netstat -tn > /tmp/ns 2>&1; then echo FLOOD""N=$(grep -c ESTABLISHED /tmp/ns); else echo FLOOD""N=none; fi; echo FLOOD""COUNT'
    if ! wait_token "FLOODCOUNT" 30; then
        fail "the console did not answer while the flood was on"
    fi
    seen=$(tr -d '\r' < "$LOG" | grep -a '^FLOODN=' | tail -1 | sed 's/^FLOODN=//')
    case "$seen" in
        ''|*[!0-9]*) fail "the guest could not list its connections: '$seen'" ;;
    esac
    established=$seen
}
attempt=0
established=0
while [ "$attempt" -lt 20 ] && [ "$established" -eq 0 ]; do
    attempt=$((attempt + 1))
    flood_count
    [ "$established" -gt 0 ] || sleep "$(scaled 1)" # interval: the next reading of a count that only the guest advances
done
if [ "$established" -eq 0 ]; then
    fail "the flood held no connection in the guest"
fi
echo "ssh-test: the flood holds $established connections in the guest: OK"
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
# The count meant something: with the forty released, the guest holds none.
attempt=0
established=1
while [ "$attempt" -lt 30 ] && [ "$established" -ne 0 ]; do
    attempt=$((attempt + 1))
    flood_count
    [ "$established" -eq 0 ] || sleep "$(scaled 1)" # interval: the next reading, while the released connections close
done
if [ "$established" -ne 0 ]; then
    fail "the guest still holds $established connections after the flood was released"
fi
echo "ssh-test: the released flood leaves the guest holding none: OK"

cleanup_boot

echo "ssh-test: root logged in by key and the test account by password, over a dropbear pty: OK"
echo "ssh-test: PASS"
