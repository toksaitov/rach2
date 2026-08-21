#!/usr/bin/env sh

#
# procfiles_test.sh: the machine's own /proc files, on the boots that differ
# and through the programs that read them.
#
# Two halves. The first is `/proc/mounts`, which is the one file here whose
# content depends on how the machine came up: this file system can be mounted
# from a disk, from the RAM module through the page-cache window, or from the
# module in place, and nothing left behind afterwards says which. So the three
# boots are run and each must name its own source and options. One boot would
# pass with a fixed line, which is what the file used to be worth.
#
# The second is the programs. `ps`, `df`, `free` and `uptime` are compiled into
# the busybox on the real image and every one of them failed on a missing file
# under /proc. Exiting 0 is necessary and not sufficient and neither is the
# output, so both are required: each applet's status is captured before
# anything reads what it wrote, and then each is asked for something
# checkable: `ps` must list the shell's own pid, which the shell
# knows independently as `$$`; `df` must report the root with a non-zero size;
# `free` must report a non-zero total; and `uptime` must print a time.
#
# Usage: procfiles_test.sh <kernel> <test.krffs> <disk.img> <os-real.krffs>
#            <user> <password> <image-mb>
#

set -e

KERNEL="$1"
TEST_IMAGE="$2"
DISK="$3"
REAL="$4"
USER_NAME="$5"
PASSWORD="$6"
# The megabytes the real image was built at, which is what df must report.
IMAGE_MB="$7"
# A host port forwarded to the guest's SSH server, so a connection can be opened
# from outside while netstat runs.
FWD_PORT=2298
PEER_PID=""

if [ -z "$KERNEL" ] || [ -z "$TEST_IMAGE" ] || [ -z "$DISK" ] || [ -z "$REAL" ] ||
        [ -z "$USER_NAME" ] || [ -z "$PASSWORD" ] || [ -z "$IMAGE_MB" ]; then
    echo "usage: procfiles_test.sh <kernel> <test.krffs> <disk.img>" >&2
    echo "                         <os-real.krffs> <user> <password> <image-mb>" >&2
    exit 2
fi

LOGS="$TEST_IMAGE.ram.log $TEST_IMAGE.window.log $TEST_IMAGE.disk.log $REAL.applets.log $REAL.applets.plain"
# shellcheck disable=SC2086
rm -f $LOGS

QPID=""

stop() {
    qpid="$QPID"
    QPID=""
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# shellcheck disable=SC2086
cleanup() {
    stop
    rm -f $LOGS
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

wait_serial() {
    i=0
    while [ "$i" -lt "$3" ]; do
        if grep -q "$2" "$1" 2>/dev/null; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# One boot of the test image under `$2` of boot options, running the probe and
# leaving its ROOTMOUNT line in `$1`. The command is re-sent because the debug
# shell drops anything typed before it is listening.
mount_boot() {
    log="$1"
    append="$2"
    shift 2
    ( sleep 8; i=0; while [ "$i" -lt 3 ]; do printf 'exec /procprobe\n'; sleep 9; i=$((i + 1)); done ) \
        | qemu-system-i386 -cpu max -display none -kernel "$KERNEL" -initrd "$TEST_IMAGE" \
        -append "$append" "$@" -serial stdio >"$log" 2>&1 &
    QPID=$!
    # Waited on the probe's own verdict, which it prints AFTER the mount line:
    # stopping at the mount line would kill the machine part way through
    # writing it, and the value read back would be a prefix.
    if ! wait_serial "$log" "PROCPROBE OK" 150; then
        stop
        echo "procfiles-test: FAIL: the probe did not pass on the boot with" >&2
        echo "procfiles-test: options '$append'" >&2
        grep -m3 "FAIL:" "$log" >&2 || true
        exit 1
    fi
    stop
}

# The mount says what it is, and each of the three says something different.
# The carriage returns the serial console writes are stripped: the value is
# compared whole, and a stray one would fail every comparison for a reason that
# has nothing to do with what was mounted.
mount_boot "$TEST_IMAGE.ram.log" ""
ram=$(sed -n 's/^PROCPROBE ROOTMOUNT //p' "$TEST_IMAGE.ram.log" | tr -d '\r' | head -1)
mount_boot "$TEST_IMAGE.window.log" "rach2.fswindow"
window=$(sed -n 's/^PROCPROBE ROOTMOUNT //p' "$TEST_IMAGE.window.log" | tr -d '\r' | head -1)
mount_boot "$TEST_IMAGE.disk.log" "rach2.fsdisk rach2.rescue" \
    -drive "file=$DISK,format=raw,if=ide,index=0,media=disk"
disk=$(sed -n 's/^PROCPROBE ROOTMOUNT //p' "$TEST_IMAGE.disk.log" | tr -d '\r' | head -1)

echo "procfiles-test: ram    : $ram"
echo "procfiles-test: window : $window"
echo "procfiles-test: disk   : $disk"

case "$ram" in
    "module / krffs rw,relatime 0 0") ;;
    *) echo "procfiles-test: FAIL: a module root did not say so" >&2; exit 1 ;;
esac
case "$window" in
    "module / krffs rw,relatime,window 0 0") ;;
    *) echo "procfiles-test: FAIL: a window root did not say so" >&2; exit 1 ;;
esac
case "$disk" in
    "/dev/disk0 / krffs rw,relatime 0 0") ;;
    *) echo "procfiles-test: FAIL: a disk root did not say so" >&2; exit 1 ;;
esac
if [ "$ram" = "$window" ] || [ "$ram" = "$disk" ]; then
    echo "procfiles-test: FAIL: two different mounts reported the same line" >&2
    exit 1
fi
echo "procfiles-test: the three mounts each name their own source: OK"

# And the programs, on the image that has them. The tokens are written split
# because the console echoes what is typed.
( sleep 12; printf '%s\n' "$USER_NAME"; sleep 2; printf '%s\n' "$PASSWORD"; sleep 4;
  printf 'echo "MYPID"":$$"\n'; sleep 2;
  printf 'ps > /tmp.ps; echo PS""-STATUS:$?; tail -6 /tmp.ps\n'; sleep 3;
  printf 'df / > /tmp.df; echo DF""-STATUS:$?; tail -1 /tmp.df\n'; sleep 3;
  printf 'free > /tmp.free; echo FREE""-STATUS:$?; cat /tmp.free\n'; sleep 3;
  printf 'uptime > /tmp.up; echo UP""-STATUS:$?; cat /tmp.up\n'; sleep 4;
  printf 'dmesg > /tmp.dm; echo DM""ESG-STATUS:$?; head -1 /tmp.dm\n'; sleep 4;
  printf 'dropbearmulti dropbear -R -g -p 22; echo SSH""D-UP\n'; sleep 10;
  printf 'route -n > /tmp.rt; echo RT""-STATUS:$?; cat /tmp.rt\n'; sleep 10;
  printf 'netstat -tn > /tmp.ns 2>/tmp.ns6; echo NS""-STATUS:$?; cat /tmp.ns; cat /tmp.ns6\n'; sleep 4;
  printf 'arp -n > /tmp.arp; echo AR""P-STATUS:$?; cat /tmp.arp\n'; sleep 6 ) \
    | qemu-system-i386 -cpu max -display none \
    -kernel "$KERNEL" -initrd "$REAL" \
    -net nic,model=rtl8139 -net "user,hostfwd=tcp::$FWD_PORT-:22" \
    -serial stdio >"$REAL.applets.log" 2>&1 &
QPID=$!
# A connection opened from OUTSIDE, held open while netstat runs, and still
# resolved in the cache when `arp` runs after it. The order matters: a machine
# that has spoken to nobody has an empty cache, and `arp` would then be asked to
# show what is not there.
# The guest sees
# its peer as QEMU's gateway, 10.0.2.2: an address the guest did not choose and
# the host end really has, which is what makes the row worth checking. The
# forward reaches the SSH server started above, so this is the shape of a live
# login and not a contrivance. The wait before it is longer than the steps that
# come first, since a connection made before the server is listening reaches
# nothing: QEMU accepts the host side of it either way.
# The server is started by hand because the image
# does not run one at boot, and QEMU's user network accepts the host side of a
# connection whether or not anything inside is listening: without a real
# listener the row would simply never appear and the check would fail for a
# reason that has nothing to do with netstat.
( sleep 42; python3 -c "
import socket, time
s = socket.create_connection(('127.0.0.1', $FWD_PORT), timeout=10)
time.sleep(40)
s.close()
" >/dev/null 2>&1 ) &
PEER_PID=$!
if ! wait_serial "$REAL.applets.log" "ARP-STATUS:" 240; then
    stop
    echo "procfiles-test: FAIL: the applet run never finished" >&2
    exit 1
fi
stop
kill "$PEER_PID" 2>/dev/null || true
wait "$PEER_PID" 2>/dev/null || true

# The console writes CRLF. Every check below reads this copy instead, so a
# trailing carriage return cannot turn a field into one that matches nothing.
PLAIN_LOG="$REAL.applets.plain"
tr -d '\r' <"$REAL.applets.log" >"$PLAIN_LOG"

# Each applet's own exit status, captured before anything reads its output. An
# applet that printed a plausible row and then failed passed the earlier version
# of this test, which looked only at the text.
for pair in "PS:ps" "DF:df" "FREE:free" "UP:uptime" "DMESG:dmesg" "RT:route" \
            "ARP:arp" "NS:netstat"; do
    token="${pair%%:*}"
    word="${pair#*:}"
    status=$(sed -n "s/^$token-STATUS:\([0-9][0-9]*\).*/\1/p" "$PLAIN_LOG" | head -1)
    if [ -z "$status" ]; then
        echo "procfiles-test: FAIL: $word never reported an exit status" >&2
        exit 1
    fi
    if [ "$status" != "0" ]; then
        echo "procfiles-test: FAIL: $word exited $status" >&2
        grep -iE "$word: " "$PLAIN_LOG" | head -2 >&2
        exit 1
    fi
    # netstat's one complaint is about `/proc/net/tcp6`, which is the intended
    # answer for a machine with no IPv6 and is asserted below; every other
    # complaint of this shape is a file a program still cannot read.
    if grep -vE "tcp6" "$PLAIN_LOG" |
            grep -qiE "$word: .*(can't open|No such file|not found)"; then
        echo "procfiles-test: FAIL: $word still cannot read what it needs:" >&2
        grep -iE "$word: " "$PLAIN_LOG" | head -2 >&2
        exit 1
    fi
done
echo "procfiles-test: ps, df, free, uptime, route, arp and netstat each exited 0: OK"

mypid=$(sed -n 's/^MYPID:\([0-9][0-9]*\).*/\1/p' "$REAL.applets.log" | tr -d '\r' | head -1)
if [ -z "$mypid" ]; then
    echo "procfiles-test: FAIL: the shell did not report its own pid" >&2
    exit 1
fi
if ! awk -v p="$mypid" '$1 == p { found = 1 } END { exit !found }' "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: ps did not list the shell's own pid ($mypid)," >&2
    echo "procfiles-test: which the shell reported independently" >&2
    exit 1
fi
echo "procfiles-test: ps lists the shell's own pid: OK"

# df's root row, checked against a number this script knows and the guest does
# not: the image was built at $IMAGE_MB megabytes, so the file system df reports
# must be that many 1K blocks. A df reading a plausible wrong number fails here,
# which "greater than zero" would not.
if ! awk -v want="$((IMAGE_MB * 1024))" \
        '$NF == "/" && $2 + 0 == want { found = 1 } END { exit !found }' "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: df did not report the root as $((IMAGE_MB * 1024))" >&2
    echo "procfiles-test: 1K blocks, which is the size this image was built at" >&2
    grep -A2 "1K-blocks" "$PLAIN_LOG" | head -4 >&2
    exit 1
fi
echo "procfiles-test: df reports the size the image was built at: OK"

if ! awk '/^Mem:/ && $2 + 0 > 0 && $2 + 0 >= $3 + 0 { found = 1 } END { exit !found }' "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: free reported no total memory" >&2
    exit 1
fi
echo "procfiles-test: free reports the machine's memory: OK"

if ! grep -qE "up +[0-9]" "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: uptime printed no time" >&2
    exit 1
fi
echo "procfiles-test: uptime prints a time: OK"

# The boot log, under the name a person reaches for. busybox's own dmesg reads
# a ring buffer through klogctl, which this kernel does not have, so /bin/dmesg
# is a script that prints /dev/bootlog and the applet is off: what this checks
# is that the name reaches the log, and the first line of the log is the banner
# the kernel prints before anything else can have written to it.
if ! grep -q "Rach2 Booting" "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: dmesg did not print the boot log" >&2
    grep -A3 "DMESG-STATUS" "$PLAIN_LOG" | head -4 >&2
    exit 1
fi
echo "procfiles-test: dmesg prints the machine's own boot log: OK"

# route: the machine's real state, not a plausible one. Under QEMU's user
# network the address is 10.0.2.15 and the gateway 10.0.2.2, both of which this
# script knows and the guest reports independently.
if ! awk '$1 == "0.0.0.0" && $2 == "10.0.2.2" { found = 1 } END { exit !found }' "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: route -n did not print the default route" >&2
    grep -A3 "Destination" "$PLAIN_LOG" | head -5 >&2
    exit 1
fi
if ! awk '$1 == "10.0.2.0" && $3 == "255.255.255.0" { found = 1 } END { exit !found }' "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: route -n did not print the on-link subnet" >&2
    exit 1
fi
echo "procfiles-test: route -n prints the real default route and subnet: OK"

# arp: the machine talked to its network to boot, so the cache holds a
# neighbour on the subnet QEMU's user network really hands out, with the MAC
# that answered. `-n` keeps the address numeric; without it busybox prints the
# name it resolved. The address is parenthesised either way, and the line is
# matched on that and on the hardware address after `at`, rather than on a field
# number, which moves depending on whether a name was found.
if ! awk '/\(10\.0\.2\.[0-9]+\)/ && / at [0-9A-Fa-f:]+ / { found = 1 } END { exit !found }' \
        "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: arp showed no resolved neighbour on the subnet" >&2
    echo "procfiles-test: this machine is actually on" >&2
    grep -A3 "HW address" "$PLAIN_LOG" | head -4 >&2
    exit 1
fi
echo "procfiles-test: arp shows a resolved neighbour on the real subnet: OK"

# netstat: the connection the guest opened outward, with the peer address the
# host end really has. Nothing in the guest chose that number.
if ! awk '$1 == "tcp" && $5 ~ /^10\.0\.2\.2:/ { found = 1 } END { exit !found }' "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: netstat did not show the outbound connection's" >&2
    echo "procfiles-test: peer as the address the peer actually has" >&2
    grep -A5 "Proto" "$PLAIN_LOG" | head -6 >&2
    exit 1
fi
echo "procfiles-test: netstat shows a live connection's real peer: OK"

# The state column, in the word netstat prints for it. The number in the file is
# Linux's and not this stack's own, so a row that reached netstat with an
# unmapped number would be named as some other state here.
if ! awk '$1 == "tcp" && $5 ~ /^10\.0\.2\.2:/ && $6 == "ESTABLISHED" { found = 1 }
          END { exit !found }' "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: netstat did not call the live connection" >&2
    echo "procfiles-test: ESTABLISHED, so the state numbers are not Linux's" >&2
    grep -A5 "Proto" "$PLAIN_LOG" | head -6 >&2
    exit 1
fi
echo "procfiles-test: netstat names the live connection's state: OK"

# And the family that is not here says so rather than being silently empty.
if ! grep -qiE "tcp6|can't open .*/proc/net/tcp6" "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: netstat said nothing about the IPv6 family," >&2
    echo "procfiles-test: which this machine does not have" >&2
    exit 1
fi
echo "procfiles-test: netstat names the IPv6 family it cannot find: OK"
echo "procfiles-test: PASS"
