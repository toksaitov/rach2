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
  printf 'uptime > /tmp.up; echo UP""-STATUS:$?; cat /tmp.up\n'; sleep 4 ) \
    | qemu-system-i386 -cpu max -display none \
    -kernel "$KERNEL" -initrd "$REAL" -serial stdio >"$REAL.applets.log" 2>&1 &
QPID=$!
if ! wait_serial "$REAL.applets.log" "UP-STATUS:" 150; then
    stop
    echo "procfiles-test: FAIL: the applet run never finished" >&2
    exit 1
fi
stop

# The console writes CRLF. Every check below reads this copy instead, so a
# trailing carriage return cannot turn a field into one that matches nothing.
PLAIN_LOG="$REAL.applets.plain"
tr -d '\r' <"$REAL.applets.log" >"$PLAIN_LOG"

# Each applet's own exit status, captured before anything reads its output. An
# applet that printed a plausible row and then failed passed the earlier version
# of this test, which looked only at the text.
for pair in "PS:ps" "DF:df" "FREE:free" "UP:uptime"; do
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
    if grep -qiE "$word: .*(can't open|No such file|not found)" "$PLAIN_LOG"; then
        echo "procfiles-test: FAIL: $word still cannot read what it needs:" >&2
        grep -iE "$word: " "$PLAIN_LOG" | head -2 >&2
        exit 1
    fi
done
echo "procfiles-test: ps, df, free and uptime each exited 0: OK"

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
echo "procfiles-test: PASS"
