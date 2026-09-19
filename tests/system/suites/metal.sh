#!/usr/bin/env sh

#
# metal_test.sh: the metal image, reached over SSH with nobody at its console.
#
# A USB controller and a keyboard are attached to the rehearsal because the
# notebook has both and this image is the one that will boot there. What can be
# rehearsed here is that the metal kernel brings the stack up and enumerates a
# device without disturbing anything else it does; what cannot is the firmware
# handoff and the chipset's port routing, which have no emulation.
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

# shellcheck source=tests/system/deadline.sh
# The runner helper, whose `until` and `gone` are the suite's waits.
RUNUTIL="$(cd "$(dirname "$0")/../../.." && pwd)/build/tests/system/runutil"
. "$(cd "$(dirname "$0")/../../.." && pwd)/tests/system/deadline.sh"
set -e

KERNEL="$1"
IMAGE="$2"
# What this suite writes under the images directory starts with `metal-test`.
OUT="$(dirname "$IMAGE")/metal-test"
PORT="${3:-2223}"

if [ -z "$KERNEL" ] || [ -z "$IMAGE" ]; then
    echo "usage: metal_test.sh <kernel> <image> [port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
KEY="$REPO/krffs/rach2_demo_ed25519"
WORK="$OUT.metal.work"
LOG="$OUT.metal.log"
KNOWN="$OUT.metal.known"
SEND="$OUT.metal.send"
BACK="$OUT.metal.back"
ACCEPT_LOG="$OUT.metal.accept.log"
rm -f "$WORK" "$LOG" "$KNOWN" "$SEND" "$BACK" "$ACCEPT_LOG"

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
    rm -f "$WORK" "$KNOWN" "$SEND" "$BACK"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    cleanup_boot
    if [ -f "$LOG" ]; then
        cp "$LOG" "$OUT.metal.fail.log" 2>/dev/null || true
    fi
    echo "metal-test: FAIL: $1" >&2
    echo "metal-test: the failing boot's console log: $OUT.metal.fail.log" >&2
    exit 1
}

fail_ssh() {
    cat "$LOG" >&2 || true
    fail "$1"
}

sh "$REPO/tests/system/accept/transport_test.sh" || fail "the acceptance transport checks"
sh "$REPO/tests/system/accept/password_test.sh" || fail "the acceptance password checks"

wait_token() {
    "$RUNUTIL" until "$LOG" "$1" "$2"
}

if [ ! -f "$KEY" ]; then
    echo "metal-test: no demo key at $KEY; run 'sh krffs/gen_demo_key.sh'" >&2
    exit 2
fi
cp "$IMAGE" "$WORK"

# The address is given on the kernel command line, as it will be on the stick,
# and it is a LAN-style one, not SLIRP's: what is under test includes
# the stack running on an address nothing compiled in. SLIRP is told to serve
# that same network, so the guest's configuration and the emulated network
# agree, which is exactly the arrangement the real machine will be in with a
# real router. -cpu max because dropbear's -R mints a host key and the kernel
# refuses untrusted entropy for it.
GUEST_NET="192.168.90.0/24"
GUEST_IP="192.168.90.15"
GUEST_GW="192.168.90.2"
# A flash drive beside the keyboard, so the acceptance script's stick section
# is rehearsed here and not first on the notebook: 48 MiB, one FAT32 volume
# labelled ACCEPT, which the script finds by that label and writes to. The
# read-only disk policy is the disk drivers' allowlist and a stick is written
# through its own driver, so the write is taken on this kernel as it would be
# on the installed one.
STICK="$WORK.stick"
dd if=/dev/zero of="$STICK" bs=1048576 count=80 2>/dev/null || fail "the stick image"
"$REPO/build/tools/image/mkmbr" --lba "$STICK" 2048:98304:0c 100352:32768:83 \
    > /dev/null || fail "mkmbr for the stick"
mformat -i "$STICK"@@1048576 -F -T 98304 -v ACCEPT :: || fail "mformat for the stick"
# The second partition is a KRFFS volume, which is what milestone 44 gave a
# stick: the acceptance script mounts it through the mass-storage driver, not
# through a loop device, and the host judges what it wrote below.
STICKVOL="$WORK.stickvol"
mkdir -p "$REPO/build/tests/system/loopimg"
printf 'hello from inside an image' > "$REPO/build/tests/system/loopimg/hello.txt"
dd if=/dev/zero of="$REPO/build/tests/system/loopimg/inner.bin" bs=512 count=8 \
    2>/dev/null
sh "$REPO/krffs/mkimage.sh" "$REPO/krffs/manifests/loopimg-test.manifest" \
    "$STICKVOL" 16 > /dev/null 2>&1 || fail "the KRFFS volume for the stick"
dd if="$STICKVOL" of="$STICK" bs=512 seek=100352 conv=notrunc 2>/dev/null \
    || fail "putting the KRFFS volume on the stick"
# Two recovery sticks beside it, each with the ISO half's entry, type 17, and
# a FAT volume labelled ACCEPT like the real one: the acceptance must take
# the stick named by its serial and refuse both of these by their tables,
# whatever their labels say. The ISO half is a few empty sectors here; the
# table is what the script reads.
for r in 1 2; do
    RSTICK="$WORK.rstick$r"
    dd if=/dev/zero of="$RSTICK" bs=1048576 count=20 2>/dev/null || fail "the recovery stick $r"
    "$REPO/build/tools/image/mkmbr" --lba "$RSTICK" 1:2047:17 2048:32768:0c \
        > /dev/null || fail "mkmbr for the recovery stick $r"
    mformat -i "$RSTICK"@@1048576 -F -T 32768 -v ACCEPT :: || fail "mformat for the recovery stick $r"
done
qemu-system-i386 -cpu max -display none -kernel "$KERNEL" \
    -initrd "$WORK" \
    -append "rach2.ip=$GUEST_IP/24 rach2.gateway=$GUEST_GW rach2.netconsole" \
    -netdev "user,id=n0,net=$GUEST_NET,host=$GUEST_GW,hostfwd=tcp::$PORT-$GUEST_IP:22" \
    -device rtl8139,netdev=n0 \
    -device qemu-xhci,id=xhci -device usb-hub,bus=xhci.0,port=1 \
    -device usb-kbd,bus=xhci.0,port=1.2 \
    -drive "if=none,id=stick,format=raw,file=$STICK" \
    -device usb-storage,bus=xhci.0,port=1.3,drive=stick,serial=ACCEPT01 \
    -drive "if=none,id=rstick1,format=raw,file=$WORK.rstick1" \
    -device usb-storage,bus=xhci.0,port=1.4,drive=rstick1,serial=RECOVERY01 \
    -drive "if=none,id=rstick2,format=raw,file=$WORK.rstick2" \
    -device usb-storage,bus=xhci.0,port=1.5,drive=rstick2,serial=RECOVERY02 \
    -serial stdio < /dev/null > "$LOG" 2>&1 &
QPID=$!

if ! wait_token "rach2 login" 90; then
    fail "the machine never reached its login prompt"
fi
if ! grep -q "IPv4 $GUEST_IP/24" "$LOG"; then
    fail "the machine did not take the address from its command line"
fi
# The kernel says which disk policy it was built with, and for this image it
# must be the one that cannot write. check_diskpolicy.sh reads the same
# string out of the artifact; this reads it out of a machine that is running.
if ! grep -q "disk: policy: read-only" "$LOG"; then
    fail "the running kernel does not report the read-only disk policy"
fi

# Nothing has been typed and nothing will be. The SSH server is up because init
# started it, or it is not up at all.
# A connection the machine refused, or that ended before the two sides had
# exchanged their identification, is tried again, three times a second
# apart: a machine slow to answer under a loaded emulator says nothing about
# what the command would have printed. A reset after that point is not
# tried again, since the command may have run and read its input, and a
# caller that sets `SSH_ONCE` gets one attempt, which the readiness loop
# does, keeping its bound its own.
ssh_run() {
    tries=0
    while :; do
        if ssh -i "$KEY" -p "$PORT" \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
            -o "ConnectTimeout=$(scaled 10)" -o BatchMode=yes \
            root@127.0.0.1 "$1" 2>"$LOG.ssh"; then
            status=0
        else
            status=$?
        fi
        cat "$LOG.ssh" >> "$LOG"
        if [ "$status" -ne 255 ] || [ -n "${SSH_ONCE:-}" ] || [ "$tries" -ge 3 ] ||
            ! grep -q 'kex_exchange_identification\|ssh_exchange_identification\|Connection refused\|Connection timed out' "$LOG.ssh"; then
            return "$status"
        fi
        tries=$((tries + 1))
        # interval: a reset before the session is tried again after a second
        sleep 1
    done
}

i=0
while [ "$i" -lt "$(scaled 60)" ]; do
    if SSH_ONCE=1 ssh_run 'echo METAL_READY' 2>/dev/null | grep -q METAL_READY; then
        break
    fi
    # interval: a try of SSH every two seconds until the server answers
    sleep 2
    i=$((i + 2))
done
if [ "$i" -ge "$(scaled 60)" ]; then
    fail "no SSH answer: init did not bring an unattended server up"
fi
echo "metal-test: the machine answered SSH with nobody at its console: OK"

# A pty, which is what an interactive session needs and what a command run
# without one never exercises.
if ! ssh -i "$KEY" -p "$PORT" -tt \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o "ConnectTimeout=$(scaled 10)" -o BatchMode=yes \
        root@127.0.0.1 'tty; exit' 2>>"$LOG" | grep -q '/dev/pts/'; then
    fail "the session did not get a pty"
fi
echo "metal-test: the session runs on a pty: OK"

# A file each way, compared by hash, not by eye. The content is a
# thousand lines so a truncation or a lost chunk cannot pass unnoticed.
i=0
while [ "$i" -lt 1000 ]; do
    echo "line $i the quick brown fox jumps over the lazy dog" >> "$SEND"
    i=$((i + 1))
done
SENT_SUM="$(cksum < "$SEND" | awk '{print $1, $2}')"

if ! ssh -i "$KEY" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN" \
        -o "ConnectTimeout=$(scaled 10)" -o BatchMode=yes \
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
i=0
AFTER=""
while [ "$i" -lt "$(scaled 60)" ]; do
    AFTER="$(ssh_run 'pidof dropbearmulti' 2>/dev/null | tr -d '\r' | awk '{print $NF}')"
    if [ -n "$AFTER" ] && ssh_run 'echo METAL_AGAIN' 2>/dev/null | grep -q METAL_AGAIN; then
        break
    fi
    AFTER=""
    # interval: a try of SSH every two seconds until the server answers
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

# The machine's own copy of its boot log, which is what makes a question about
# a boot answerable after that boot without anything arranged in advance.
BOOTLOG="$(ssh_run 'cat /dev/bootlog' | tr -d '\r')"
if [ -z "$BOOTLOG" ]; then
    fail "/dev/bootlog gave nothing, so the machine cannot be asked what it printed"
fi
for want in "Rach2 Booting" "Initializing system components" "net: ready"; do
    echo "$BOOTLOG" | grep -q "$want" || fail "the fetched boot log does not contain '$want'"
done
echo "metal-test: the machine handed over its own boot log, from the first line: OK"

# The acceptance script itself, rehearsed here so it is not being debugged on
# the laptop. Its baseline comparisons are against a drive this machine does
# not have, which it reports, not fails on.
RACH2_ACCEPT_PASSWORD=$(cat "$REPO/krffs/rach2_demo_password")
export RACH2_ACCEPT_PASSWORD
SU_COMMAND="$REPO/tests/system/accept/su_command.sh"
TEST_KEY="$REPO/krffs/rach2_test_ed25519"
rc=0
GOT="$(sh "$SU_COMMAND" "$TEST_KEY" 127.0.0.1 "$PORT" test "$KNOWN" \
    "printf '%s\\n' \"a 'quote' and a dollar \\\$\"; exit 7")" || rc=$?
if [ "$rc" -ne 7 ] || [ "$GOT" != "a 'quote' and a dollar \$" ]; then
    fail "the su transport changed a quoted command, its output or its status"
fi
echo "metal-test: su preserves a quoted command and its nonzero status: OK"
stick_before=$(shasum -a 256 "$STICK")
UNNAMED="$(sh "$REPO/tests/system/accept/metal_accept.sh" 127.0.0.1 "$PORT" 2>&1)" || true
if ! echo "$UNNAMED" | grep -q '^metal-accept: PASS' ||
    ! echo "$UNNAMED" | grep -q 'was not named by the owner and is not written' ||
    echo "$UNNAMED" | grep -q 'is the one written'; then
    echo "$UNNAMED" >&2
    fail "the acceptance selected a stick without a serial"
fi
[ "$stick_before" = "$(shasum -a 256 "$STICK")" ] || fail "the unnamed stick was changed"
echo "metal-test: acceptance without a serial leaves the stick unchanged: OK"
accept_status=0
RACH2_ACCEPT_SU=1 \
    RACH2_ACCEPT_USER=test RACH2_ACCEPT_KEY="$TEST_KEY" \
    sh "$REPO/tests/system/accept/metal_accept.sh" 127.0.0.1 "$PORT" ACCEPT01 \
    > "$ACCEPT_LOG" 2>&1 || accept_status=$?
unset RACH2_ACCEPT_PASSWORD
if [ "$accept_status" -ne 0 ] || ! grep -q '^metal-accept: PASS' "$ACCEPT_LOG"; then
    cat "$ACCEPT_LOG" >&2
    fail "the acceptance script did not pass against the rehearsal machine (status $accept_status)"
fi
echo "metal-test: the acceptance script runs clean against the rehearsal machine: OK"
if grep -q 'disk: disk 0 usb ' "$ACCEPT_LOG" &&
    grep -q 'disk: identity 0 512 removable free ACCEPT01' "$ACCEPT_LOG" &&
    grep -q 'disk: the MBR hashes to ' "$ACCEPT_LOG"; then
    echo "metal-test: and its disk row reads the registry without the boot diagnostic: OK"
else
    cat "$ACCEPT_LOG" >&2
    fail "the acceptance script's disk row did not read the registry"
fi
if grep -q 'the file system in that file mounts and reads' "$ACCEPT_LOG"; then
    echo "metal-test: and its image row attached a file, mounted it and read it: OK"
else
    cat "$ACCEPT_LOG" >&2
    fail "the acceptance script's image row did not run against the rehearsal machine"
fi
if grep -q 'stick: the volume ACCEPT on /dev/disk[0-9]*p[0-9]* mounts' "$ACCEPT_LOG"; then
    echo "metal-test: and its stick section found the stick, wrote it and flushed it: OK"
else
    fail "the acceptance script's stick section did not run against the stick"
fi
if [ "$(grep -c 'carries a Rach2 recovery ISO in its table and is not written' "$ACCEPT_LOG")" = 2 ] &&
    grep -q 'stick: /dev/disk[0-9]* (ACCEPT01) is the one written' "$ACCEPT_LOG"; then
    echo "metal-test: and it took the stick named by its serial and refused both recovery sticks by their tables: OK"
else
    grep 'stick:' "$ACCEPT_LOG" >&2
    fail "the acceptance script did not refuse the recovery sticks and take the named one"
fi
if grep -q 'stick: the KRFFS volume on .* mounts, reads and takes a write' "$ACCEPT_LOG"; then
    echo "metal-test: and its KRFFS section mounted a volume of the stick and wrote it: OK"
else
    fail "the acceptance script's KRFFS section did not run against the stick"
fi
# What the machine wrote to that volume, judged here by the host's own tools.
dd if="$STICK" of="$STICK.vol" bs=512 skip=100352 count=32768 2>/dev/null \
    || fail "taking the KRFFS volume off the stick"
"$REPO/build/krffs/fsck.krffs" "$STICK.vol" --strict > "$STICK.fsck" 2>&1 \
    || { cat "$STICK.fsck" >&2; fail "the KRFFS volume of the stick is not clean"; }
"$REPO/build/krffs/readfile.krffs" "$STICK.vol" /accept.txt "$STICK.accept" \
    > /dev/null 2>&1 || fail "reading what the machine wrote to the stick's volume"
grep -q written-on-the-stick "$STICK.accept" \
    || fail "the bytes the machine wrote to the stick's volume"
echo "metal-test: and the host finds that volume clean with the machine's bytes in it: OK"
if grep -q 'behind a hub: port' "$ACCEPT_LOG" &&
    grep -q 'hub: device [0-9]*: [0-9]* ports' "$ACCEPT_LOG"; then
    echo "metal-test: and its hub section found the hub and the devices behind it: OK"
else
    fail "the acceptance script's hub section did not run against the hub"
fi

dd if="$STICK" of="$WORK.mbr" bs=512 count=1 2>/dev/null || fail "the host's MBR read"
[ "$(wc -c < "$WORK.mbr" | tr -d ' ')" = 512 ] || fail "the host's MBR read was short"
mbr_sum=$(shasum -a 256 "$WORK.mbr") || fail "hashing the host's MBR"
mbr_sum=${mbr_sum%% *}
grep -q "disk: the MBR hashes to $mbr_sum" "$ACCEPT_LOG" \
    || fail "the acceptance did not hash the same sector as the host"

# Mutate only a private host script. The production reader has no override.
# The copy lives under the images directory with this suite's name: a
# directory made under build/tests changes that directory's time, which the
# test image depends on, and a pooled suite would restage it.
ACCEPT_ROOT=$(mktemp -d "$OUT.accept.XXXXXX")
trap 'cleanup; rm -rf "$ACCEPT_ROOT"' EXIT
ACCEPT_COPY="$ACCEPT_ROOT/tests/system/accept"
mkdir -p "$ACCEPT_COPY"
for directory in build krffs; do
    ln -s "$REPO/$directory" "$ACCEPT_COPY/../../../$directory"
done
cp "$REPO/tests/system/accept/metal_accept.sh" "$ACCEPT_COPY/original.sh"
cp "$REPO/tests/system/accept/su_command.sh" "$ACCEPT_COPY/su_command.sh"
# shellcheck disable=SC2016 # Match the guest variable in the private source.
sed 's@/bin/busybox dd if=/dev/disk0 of="\$f" bs=512 count=1@/bin/busybox dd if=/dev/disk0 of="$f" bs=512 count=0@' \
    "$ACCEPT_COPY/original.sh" > "$ACCEPT_COPY/metal_accept.sh"
cmp -s "$ACCEPT_COPY/original.sh" "$ACCEPT_COPY/metal_accept.sh" \
    && fail "the empty-read mutation did not change the private script"
short_status=0
sh "$ACCEPT_COPY/metal_accept.sh" 127.0.0.1 "$PORT" \
    > "$WORK.short-mbr.log" 2>&1 || short_status=$?
if [ "$short_status" -ne 1 ] ||
    ! grep -q 'FAIL: disk: the published whole-disk window did not supply one complete sector' "$WORK.short-mbr.log" ||
    grep -q 'disk: the MBR hashes to ' "$WORK.short-mbr.log"; then
    cat "$WORK.short-mbr.log" >&2
    fail "the acceptance did not reject an empty MBR read"
fi
echo "metal-test: acceptance rejects a successful read of zero MBR bytes: OK"
cp "$ACCEPT_COPY/original.sh" "$ACCEPT_COPY/metal_accept.sh"
restored_status=0
sh "$ACCEPT_COPY/metal_accept.sh" 127.0.0.1 "$PORT" \
    > "$WORK.restored-mbr.log" 2>&1 || restored_status=$?
if [ "$restored_status" -ne 0 ] ||
    ! grep -q '^metal-accept: PASS' "$WORK.restored-mbr.log" ||
    ! grep -q "disk: the MBR hashes to $mbr_sum" "$WORK.restored-mbr.log"; then
    cat "$WORK.restored-mbr.log" >&2
    fail "the acceptance did not pass after the MBR reader was restored"
fi
echo "metal-test: the restored MBR read matches the host's sector and acceptance passes: OK"

ssh_run 'cat /dev/bootlog' > "$WORK.final-bootlog" \
    || fail_ssh "reading the live boot log after acceptance traffic"
for want in 'boot: Rach2 Booting' 'usb: controller:' 'lapic:'; do
    grep -q "$want" "$WORK.final-bootlog" \
        || fail "the live boot log lost '$want' during acceptance traffic"
done
echo "metal-test: the live boot log retains the early inventory after acceptance traffic: OK"

cleanup_boot
echo "metal-test: PASS"
