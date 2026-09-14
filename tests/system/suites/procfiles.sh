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

# shellcheck source=tests/system/deadline.sh
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

KERNEL="$1"
TEST_IMAGE="$2"
DISK="$3"
REAL="$4"
USER_NAME="$5"
PASSWORD="$6"
# The megabytes the real image was built at, which is what df must report.
IMAGE_MB="$7"
# A host port forwarded to the guest's SSH server, so a connection can be opened
# from outside while netstat runs. Taken as an argument, as every other suite
# takes its port, so that the one place the ports are listed is the Makefile:
# two suites forwarding one port cannot both run.
FWD_PORT="${8:-2299}"
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
FEED="$TEST_IMAGE.procfiles.feed"

# The console is driven through tests/system/drive.sh: a line is typed when
# the output it answers has arrived, under a bound.
# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"

stop() {
    qpid="$QPID"
    QPID=""
    drive_stop
    if [ -n "$qpid" ]; then
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
    rm -f "$FEED"
}

# A machine on the fifo and the log, and the feeder that types at it.
boot_driven() {
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none "$@" -serial stdio < "$FEED" > "$DRIVE_LOG" 2>&1 &
    QPID=$!
    DRIVE_FIFO=$FEED
    drive_feed "$QPID"
}

# The logs are removed only after a pass: a failed row leaves them for the
# reader, since the failure's explanation is in them.
# shellcheck disable=SC2086
cleanup() {
    stop
    if [ -n "$PASSED" ]; then
        rm -f $LOGS
    fi
}
PASSED=""


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

# One boot of the test image under `$2` of boot options, running the probe in
# its mount-only mode and leaving its ROOTMOUNT line in `$1`. The command is
# typed once the debug shell's prompt has been printed.
mount_boot() {
    log="$1"
    append="$2"
    shift 2
    DRIVE_LOG=$log
    boot_driven -kernel "$KERNEL" -initrd "$TEST_IMAGE" -append "$append" "$@"
    if ! drive_expect "Type 'help'" 60; then
        stop
        echo "procfiles-test: FAIL: no debug shell on the boot with options '$append'" >&2
        exit 1
    fi
    drive_type 'exec /procprobe mount'
    # Waited on the probe's own verdict, which it prints AFTER the mount line:
    # stopping at the mount line would kill the machine part way through
    # writing it, and the value read back would be a prefix.
    if ! wait_serial "$log" "PROCPROBE OK" "$(scaled 150)"; then
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
    "/dev/disk0p1 / krffs rw,relatime 0 0") ;;
    *) echo "procfiles-test: FAIL: a disk root did not say so" >&2; exit 1 ;;
esac
if [ "$ram" = "$window" ] || [ "$ram" = "$disk" ]; then
    echo "procfiles-test: FAIL: two different mounts reported the same line" >&2
    exit 1
fi
echo "procfiles-test: the three mounts each name their own source: OK"

# And the programs, on the image that has them. The tokens are written split
# because the console echoes what is typed; each line is typed once the one
# before it has answered with its token.
DRIVE_LOG="$REAL.applets.log"
boot_driven -kernel "$KERNEL" -initrd "$REAL" \
    -net nic,model=rtl8139 -net "user,hostfwd=tcp::$FWD_PORT-:22"
# The seconds below are the plain ones: `drive_expect`, `drive_prompt` and
# `drive_login` scale what they are given. Scaling them here as well made a
# 60-second bound 64 minutes on a busy host, and on 2026-09-14 a machine that
# stopped answering held a whole set for an hour before that bound could fire.
applet() { # applet LINE TOKEN: the line, its token, and the prompt after its output
    drive_type "$1"
    if ! drive_expect "$2" 60 || ! drive_prompt 60; then
        stop
        echo "procfiles-test: FAIL: the applet run stopped before $2" >&2
        exit 1
    fi
}
if ! drive_login "$USER_NAME" "$PASSWORD" 90; then
    stop
    echo "procfiles-test: FAIL: the real image did not log its user in" >&2
    exit 1
fi
applet 'echo "MYPID"":$$"' 'MYPID:'
applet 'ps > /tmp.ps; echo PS""-STATUS:$?; tail -6 /tmp.ps' 'PS-STATUS:'
applet 'df / > /tmp.df; echo DF""-STATUS:$?; tail -1 /tmp.df' 'DF-STATUS:'
applet 'free > /tmp.free; echo FREE""-STATUS:$?; cat /tmp.free' 'FREE-STATUS:'
applet 'uptime > /tmp.up; echo UP""-STATUS:$?; cat /tmp.up' 'UP-STATUS:'
applet 'dmesg > /tmp.dm; echo DM""ESG-STATUS:$?; sed -n "1s/^/DM""ESG-FIRST:/p" /tmp.dm' 'DMESG-FIRST:'
applet 'grep -c "^<[0-9]>\\[" /tmp.dm | sed s/^/DM""ESG-RECORDS:/' 'DMESG-RECORDS:'
applet 'grep -c "Type .help." /tmp.dm | sed s/^/DM""ESG-CONSOLE:/' 'DMESG-CONSOLE:'
applet 'dropbearmulti dropbear -R -g -p 22; echo SSH""D-UP' 'SSHD-UP'
# A connection opened from OUTSIDE, held open while netstat runs, and still
# resolved in the cache when `arp` runs after it. The order matters: a machine
# that has spoken to nobody has an empty cache, and `arp` would then be asked to
# show what is not there.
# The guest sees
# its peer as QEMU's gateway, 10.0.2.2: an address the guest did not choose and
# the host end really has, which is what makes the row worth checking. The
# forward reaches the SSH server started above, so this is the shape of a live
# login and not a contrivance. It is opened once the server has been started,
# since a connection made before the server is listening reaches nothing:
# QEMU accepts the host side of it either way. The server is started by hand
# because the image does not run one at boot. The seconds after the open are
# an interval: the host's connection is accepted by QEMU at once and reaches
# the guest's server, and its table, a moment later.
"$REPO/build/tests/system/hostpeer" hold "$FWD_PORT" 90 >/dev/null 2>&1 &
PEER_PID=$!
sleep 3
applet 'route -n > /tmp.rt; echo RT""-STATUS:$?; cat /tmp.rt' 'RT-STATUS:'
applet 'netstat -tn > /tmp.ns 2>/tmp.ns6; echo NS""-STATUS:$?; cat /tmp.ns; cat /tmp.ns6' 'NS-STATUS:'
applet 'arp -n > /tmp.arp; echo AR""P-STATUS:$?; cat /tmp.arp' 'ARP-STATUS:'
applet 'ifconfig eth0 > /tmp.if; echo IF""-STATUS:$?; cat /tmp.if' 'IF-STATUS:'
applet 'cat /proc/net/dev > /tmp.nd; echo ND""-STATUS:$?; cat /tmp.nd' 'ND-STATUS:'
applet 'cat /proc/interrupts > /tmp.ir; echo IR""-STATUS:$?; cat /tmp.ir' 'IR-STATUS:'
applet 'grep "^intr" /proc/stat | sed s/^/IN""TR:/' 'INTR:intr'
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
for pair in "PS:ps" "DF:df" "FREE:free" "UP:uptime" "ND:cat" "IR:cat" "IF:ifconfig" \
            "DMESG:dmesg" "RT:route" "ARP:arp" "NS:netstat"; do
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
echo "procfiles-test: ps, df, free, uptime, cat, ifconfig, dmesg, route, arp and netstat each exited 0: OK"

# RUNNING means the link is up, not that a card is bound. The distinction is
# only visible from outside the machine when it is wrong: a driver that read
# its link bit the wrong way round would drop this flag on a machine whose
# network plainly works, which is what the rest of this suite has just used.
if ! grep -q "RUNNING" "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: ifconfig does not report the interface RUNNING" >&2
    sed -n '/^eth0/,/^$/p' "$PLAIN_LOG" | head -6 >&2
    exit 1
fi
echo "procfiles-test: ifconfig reports the interface RUNNING, so the link is read: OK"

# /proc/net/dev: the sixteen counters, by position. Field count is checked as
# well as content, because a column added or lost still prints a plausible
# line, and every program that reads this file finds its number by counting.
# Six of the sixteen are kept by this machine; the other ten are zero, and a
# machine that has spoken must show that in the first of each group, which is
# why this is read last: the counters are empty until the held connection above
# has put a packet on the wire in each direction.
nd_row=$(sed -n 's/^ *eth0: *//p' "$PLAIN_LOG" | head -1)
if [ -z "$nd_row" ]; then
    echo "procfiles-test: FAIL: /proc/net/dev has no eth0 row" >&2
    exit 1
fi
nd_fields=$(printf '%s\n' "$nd_row" | wc -w | tr -d ' ')
if [ "$nd_fields" != "16" ]; then
    echo "procfiles-test: FAIL: /proc/net/dev eth0 has $nd_fields fields, not 16" >&2
    echo "  $nd_row" >&2
    exit 1
fi
for f in $nd_row; do
    case "$f" in
        *[!0-9]*)
            echo "procfiles-test: FAIL: /proc/net/dev field '$f' is not a number" >&2
            exit 1 ;;
    esac
done
nd_rx_bytes=$(printf '%s\n' "$nd_row" | cut -d' ' -f1)
nd_tx_bytes=$(printf '%s\n' "$nd_row" | cut -d' ' -f9)
if [ "$nd_rx_bytes" = "0" ] || [ "$nd_tx_bytes" = "0" ]; then
    echo "procfiles-test: FAIL: a machine that has used its network reports" >&2
    echo "  no bytes either way: $nd_row" >&2
    exit 1
fi
echo "procfiles-test: /proc/net/dev prints sixteen numeric counters and has carried traffic: OK"

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

# The boot log, under the name a person expects. busybox's own dmesg reads a
# ring buffer through klogctl, which this kernel does not have, so /bin/dmesg
# is a script that prints /dev/bootlog and the applet is off. The guest tags
# the first line of what the command printed, because the same banner is on
# this console anyway: a check that grepped the transcript for it would pass
# with an empty log and a `cat` that read nothing.
if ! grep -q "^DMESG-FIRST:.*Rach2 Booting" "$PLAIN_LOG"; then
    echo "procfiles-test: FAIL: dmesg did not print the boot log" >&2
    grep -A3 "DMESG-STATUS" "$PLAIN_LOG" | head -4 >&2
    exit 1
fi
echo "procfiles-test: dmesg prints the machine's own boot log: OK"

# What the log holds, now that it is the kernel's records and not every byte
# that crossed the serial port: whole records, and none of the shell's own
# output. The counts are taken on the machine, so an empty file fails both.
records="$(sed -n 's/^DMESG-RECORDS://p' "$PLAIN_LOG" | tr -d "\r" | head -1)"
console="$(sed -n 's/^DMESG-CONSOLE://p' "$PLAIN_LOG" | tr -d "\r" | head -1)"
case "$records" in
    ''|*[!0-9]*) records=0 ;;
esac
if [ "$records" -lt 20 ]; then
    echo "procfiles-test: FAIL: the boot log holds $records records" >&2
    exit 1
fi
if [ "$console" != "0" ]; then
    echo "procfiles-test: FAIL: the boot log holds the shell's own output" >&2
    exit 1
fi
echo "procfiles-test: the boot log holds $records records and no console text: OK"

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

# /proc/interrupts: the timer's own line, which every boot has. The count is
# read from the machine and must be a number that has moved, the controller
# column says which of the two paths the vector arrives on, and the name is the
# one the handler registered: a file that printed the vector and the count
# alone would pass a check on either of them by itself.
pit_row=$(awk -F"\t" '$1 == "32:" { print $2 "|" $3 "|" $4; exit }' "$PLAIN_LOG")
if [ -z "$pit_row" ]; then
    echo "procfiles-test: FAIL: /proc/interrupts has no line for vector 32" >&2
    grep -A6 "IR-STATUS" "$PLAIN_LOG" | head -8 >&2
    exit 1
fi
pit_count="${pit_row%%|*}"
pit_rest="${pit_row#*|}"
pit_path="${pit_rest%%|*}"
pit_name="${pit_rest#*|}"
case "$pit_count" in
    ''|*[!0-9]*)
        echo "procfiles-test: FAIL: vector 32's count is not a number: $pit_row" >&2
        exit 1 ;;
esac
if [ "$pit_count" -lt 100 ]; then
    echo "procfiles-test: FAIL: the timer has ticked $pit_count times on a" >&2
    echo "procfiles-test: machine that has been up for seconds" >&2
    exit 1
fi
if [ "$pit_path" != "PIC" ] || [ "$pit_name" != "pit" ]; then
    echo "procfiles-test: FAIL: vector 32 is '$pit_path' handled by '$pit_name'," >&2
    echo "procfiles-test: not the timer on the 8259 pair" >&2
    exit 1
fi
echo "procfiles-test: /proc/interrupts counts the timer's vector and names it: OK"

# /proc/stat's intr line, which said zero on every machine before the counts
# existed. It is every interrupt the kernel has routed, so it cannot be below
# the sum of the per-vector counts printed a moment earlier in the same boot.
# Comparing it against the timer's line alone would pass for a file that
# reported the timer and nothing else.
intr=$(sed -n 's/^INTR:intr \([0-9][0-9]*\).*/\1/p' "$PLAIN_LOG" | head -1)
if [ -z "$intr" ]; then
    echo "procfiles-test: FAIL: /proc/stat has no intr line" >&2
    exit 1
fi
vector_sum=$(awk -F"\t" '$1 ~ /^[0-9]+:$/ { total += $2 + 0 } END { print total + 0 }' "$PLAIN_LOG")
if [ "$vector_sum" -lt "$pit_count" ]; then
    echo "procfiles-test: FAIL: the per-vector counts sum to $vector_sum, below the" >&2
    echo "procfiles-test: $pit_count the timer's own line reports" >&2
    exit 1
fi
if [ "$intr" -lt "$vector_sum" ]; then
    echo "procfiles-test: FAIL: /proc/stat reports $intr interrupts, fewer than the" >&2
    echo "procfiles-test: $vector_sum the vectors' own lines add up to" >&2
    exit 1
fi
echo "procfiles-test: /proc/stat counts at least what the vectors' lines add up to ($intr against $vector_sum): OK"
PASSED=1
echo "procfiles-test: PASS"
