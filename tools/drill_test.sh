#!/usr/bin/env sh

#
# drill_test.sh: the recovery drill, end to end on one disk.
#
# This is the day the machine's root will not mount. Nobody is going to open the
# case, so the repair has to arrive over the network, and the only thing that
# can write a partition nothing is mounting is a kernel that boots from
# somewhere else. That is what partition 3 has: the recovery build, which is
# the only one whose /dev/disk0 accepts a write, and a RAM image with an SSH
# server and two tools in it.
#
# Five boots of one disk. The first is the machine as it shipped, and it proves
# the everyday kernel cannot write its own disk: `reflash` is refused, a shell
# redirect into the device is refused, and the image is compared byte for byte
# afterwards so the refusal is shown to have written nothing, not merely
# to have printed something. Then the root's first sector is destroyed from the
# host, with the machine stopped, and the second boot must fail to mount rather
# than answer anything. The third arms the recovery entry in the environment
# block and boots it: recovery comes up on partition 3, takes an address, and
# answers SSH with nothing typed at the console. The whole root partition then
# arrives through one `ssh` pipe, and the region is read back through the same
# device and hashed on the machine against the hash the host computed. The last
# boot is the ordinary entry again, which must mount and offer a login.
#
# Usage: drill_test.sh <grub-disk> <root password> <ssh port>
#

set -e

DISK="$1"
ROOTPASSWORD="$2"
PORT="${3:-2298}"

if [ -z "$DISK" ] || [ -z "$ROOTPASSWORD" ]; then
    echo "usage: drill_test.sh <grub-disk> <root password> [ssh port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEADLINE=2400
START="$(date +%s)"

WORK="$DISK.work"
FEED="$DISK.feed"
STEPS="$DISK.steps"
ROOTIMG="$DISK.root.img"
KNOWN="$DISK.known"
LOG=""
QPID=""
FPID=""

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

cleanup() {
    cleanup_boot
    rm -f "$WORK" "$FEED" "$STEPS" "$ROOTIMG" "$KNOWN" "$DISK".log* \
        "$WORK.sector0" "$DISK.sector0" "$WORK.save"
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
        echo "drill-test: the failing boot's console log: $DISK.fail.log" >&2
    fi
    cleanup_boot
    echo "drill-test: FAIL: $1" >&2
    exit 1
}

# Wait up to $3 seconds until the log has at least $2 occurrences of $1.
wait_count() {
    i=0
    while [ "$i" -lt "$3" ]; do
        n="$(grep -ac "$1" "$LOG" 2>/dev/null)" || true
        if [ "${n:-0}" -ge "$2" ]; then
            return 0
        fi
        if [ "$(($(date +%s) - START))" -gt "$DEADLINE" ]; then
            fail "the run passed its ${DEADLINE}s deadline waiting for $1"
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# step <pattern> <count> <command>: the feeder sends <command> once the log
# has at least <count> occurrences of <pattern>.
step() {
    printf '%s\t%s\t%s\n' "$2" "$1" "$3" >> "$STEPS"
}

# Boot $WORK on the current $STEPS, with the SSH port forwarded when $1 is
# `net`; $LOG must be set by the caller.
boot_feed() {
    rm -f "$FEED"
    mkfifo "$FEED"
    if [ "${1:-}" = "net" ]; then
        qemu-system-i386 -cpu max -display none -rtc base=utc \
            -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
            -netdev "user,id=n0,hostfwd=tcp::$PORT-:22" -device rtl8139,netdev=n0 \
            -serial stdio < "$FEED" > "$LOG" 2>&1 &
    else
        qemu-system-i386 -cpu max -display none -rtc base=utc \
            -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
            -serial stdio < "$FEED" > "$LOG" 2>&1 &
    fi
    QPID=$!
    (
        while IFS='	' read -r count pattern command; do
            waited=0
            while [ "$waited" -lt 600 ]; do
                seen="$(grep -ac "$pattern" "$LOG" 2>/dev/null)" || true
                if [ "${seen:-0}" -ge "$count" ]; then
                    break
                fi
                if [ "$(($(date +%s) - START))" -gt "$DEADLINE" ]; then
                    break
                fi
                sleep 1
                waited=$((waited + 1))
            done
            seen="$(grep -ac "$pattern" "$LOG" 2>/dev/null)" || true
            if [ "${seen:-0}" -lt "$count" ]; then
                break
            fi
            printf '%s\n' "$command"
        done < "$STEPS"
        held=0
        while [ "$held" -lt 900 ] && kill -0 "$QPID" 2>/dev/null; do
            sleep 1
            held=$((held + 1))
        done
    ) > "$FEED" &
    FPID=$!
}

# A watch-only boot: no feed, a bounded wait for $1.
boot_watch() {
    rm -f "$FEED"
    mkfifo "$FEED"
    qemu-system-i386 -cpu max -display none -rtc base=utc \
        -drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        -serial stdio < "$FEED" > "$LOG" 2>&1 &
    QPID=$!
    (sleep 120) > "$FEED" &
    FPID=$!
    wait_count "$1" 1 100 || fail "the boot never reached $1"
    cleanup_boot
}

# ssh into the machine as root by key, with nothing of the runner's own offered.
machine() {
    ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o ConnectTimeout=20 -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$REPO/krffs/rach2_demo_ed25519" -p "$PORT" root@localhost "$1" 2>&1
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

rm -f "$WORK" "$KNOWN"
cp "$DISK" "$WORK"

# The root partition's extent, read off the disk's own table rather than
# recomputed from the sizes the build was given: what is about to be destroyed
# and rewritten has to be the partition the machine actually mounts.
EXTENT="$(python3 -c "
import struct, sys
d = open('$WORK', 'rb').read(512)
for i in range(4):
    e = d[446 + 16 * i:446 + 16 * i + 16]
    if e[4] == 0x83:
        start, count = struct.unpack_from('<II', e, 8)
        print(start * 512, count * 512)
        sys.exit(0)
sys.exit('drill-test: the disk has no KRFFS partition entry')
")"
ROOT_OFF="$(echo "$EXTENT" | awk '{print $1}')"
ROOT_BYTES="$(echo "$EXTENT" | awk '{print $2}')"
if [ -z "$ROOT_OFF" ] || [ -z "$ROOT_BYTES" ]; then
    fail "the root partition's extent could not be read from the table"
fi

# The image that will be streamed back in: the root as it was built, taken from
# the disk before anything touches it.
dd if="$WORK" of="$ROOTIMG" bs=512 skip=$((ROOT_OFF / 512)) count=$((ROOT_BYTES / 512)) \
    status=none
ROOT_SHA="$(sha256_of "$ROOTIMG")"

# An everyday artifact offered as recovery must be rejected by the check that
# reads the authority off a built kernel, or a mis-built one would reach the day
# it is needed unnoticed.
if python3 "$REPO/tools/check_recovery.py" "$REPO/build/rach2/kernel" recovery \
        > /dev/null 2>&1; then
    fail "the everyday kernel passed the recovery artifact check"
fi

# First, the recovery build's own rule, on its own: it publishes the whole-disk
# window with nothing on the command line asking for it. The disk boots this
# kernel from a menu entry nobody will edit on the day it is needed, so a build
# that waited to be asked would be a build that could not repair anything. This
# boot mounts nothing from the disk and writes nothing to it.
if ! python3 "$REPO/tools/harness.py" --kernel "$REPO/build/rach2-recovery/kernel" \
        --initrd "$REPO/build/images/rach2-recovery.krffs" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        --append "rach2.root=ram" --timeout 180 --steptmo 60 --ok "NODETHERE" \
        "expect=rach2 login:" "send=root" "expect=Password:" "send=$ROOTPASSWORD" \
        "expect=# " "send=test -c /dev/disk0 && echo NODE\"\"THERE" \
        "expect=NODETHERE" wait=3 > /dev/null 2>&1; then
    fail "the recovery kernel did not publish /dev/disk0 without rach2.diskdiag"
fi

# Boot 1: the machine as it shipped. The everyday kernel must refuse to write
# its own disk, and the refusal must have written nothing.
LOG="$DISK.log.refuse"
: > "$STEPS"
step "rach2 login:" 1 "root"
step "Password:" 1 "$ROOTPASSWORD"
# Every marker here is a line only the machine can produce. This image runs the
# ISH shell, which does not take the quotes out of an echoed token, so a token
# typed at it would appear in the log whether or not anything happened.
step "The ISH shell" 1 "reflash /dev/disk0 0 512 < /dev/zero"
step "did not take the write" 1 "cat /etc/motd > /dev/disk0"
step "cannot open a redirection" 1 "halt"
boot_feed
wait_count "cannot open a redirection" 1 300 || fail "the refusal boot never finished"
sleep 3
cleanup_boot
grep -aq "did not take the write" "$LOG" || \
    fail "reflash was not refused by the everyday kernel"
grep -aq "cannot open a redirection" "$LOG" || \
    fail "a shell redirect into the whole-disk device was not refused"
# Both refused writes aimed at the first sector, and that is what is compared:
# the machine mounted its root read-write on the way to the prompt, so the
# partition it is running on is not the same bytes it started as, but sector 0
# is nobody's to touch and has the partition table an accident here would
# destroy.
dd if="$WORK" of="$WORK.sector0" bs=512 count=1 status=none
dd if="$DISK" of="$DISK.sector0" bs=512 count=1 status=none
if ! cmp -s "$WORK.sector0" "$DISK.sector0"; then
    rm -f "$WORK.sector0" "$DISK.sector0"
    fail "the refused writes changed the first sector of the disk"
fi
rm -f "$WORK.sector0" "$DISK.sector0"

# The root's first sector, destroyed with the machine stopped: this is the
# failure the drill exists for, and it is made, not waited for.
python3 -c "
f = open('$WORK', 'r+b')
f.seek($ROOT_OFF)
f.write(b'\x00' * 512)
f.close()
"

# Boot 2: the ordinary entry, which must now fail to mount and offer nothing.
LOG="$DISK.log.broken"
boot_watch "no KRFFS partition on the disk"
if grep -aq "rach2 login:" "$LOG"; then
    fail "a machine whose root is destroyed still reached a login"
fi

# Boot 3: recovery, armed from the host the way an operator would arm it from
# the console, since a machine that cannot mount its root cannot arm anything
# for itself.
python3 -c "
d = open('$WORK', 'rb').read()
i = d.find(b'# GRUB Environment Block')
if i < 0:
    raise SystemExit('drill-test: the disk has no environment block')
block = (b'# GRUB Environment Block\n'
         b'# WARNING: Do not edit this file by tools other than grub-editenv!!!\n'
         b'next_entry=recovery\n').ljust(1024, b'#')
f = open('$WORK', 'r+b')
f.seek(i)
f.write(block)
f.close()
"

LOG="$DISK.log.recovery"
: > "$STEPS"
boot_feed net
wait_count "rach2 login:" 1 200 || fail "the recovery entry never reached a login"
# Nothing is typed at that console: the image starts its own server, and the
# lease and the key login are what say the machine is reachable.
wait_count "lease of" 1 90 || fail "recovery never took an address"

OUT="$(machine 'test -c /dev/disk0 && echo DISK""NODE' || true)"
case "$OUT" in
    *DISKNODE*) ;;
    *) fail "recovery has no whole-disk device: $OUT" ;;
esac

# A write that would run past the end is refused before the first byte, which
# is the whole reason the length is an argument. The length is a whole number of
# sectors, so what refuses it is the bound and not the alignment.
OUT="$(machine "reflash /dev/disk0 $ROOT_OFF 999999999488 < /dev/zero" || true)"
case "$OUT" in
    *"past the end"*) ;;
    *) fail "a write past the end of the device was not refused: $OUT" ;;
esac

# And the record of a fixed address does not outlive the address. A machine
# rescued on a network with no server can be put back on DHCP by hand, and the
# lease script is where the address changes hands, so that is where the record
# goes. The script is run directly on its `bound` case, not through a
# client, for two reasons: the sequence needs a server, which the isolated leg
# by definition has not got, and a real client begins by taking the address off,
# which would end this session before it could look.
OUT="$(machine 'echo 1.2.3.4 > /var/run/rescue-static; interface=eth0 ip=10.0.2.15 subnet=255.255.255.0 router=10.0.2.2 /usr/share/udhcpc/default.script bound > /dev/null 2>&1; test ! -f /var/run/rescue-static && echo MARKER""GONE' || true)"
case "$OUT" in
    *MARKERGONE*) ;;
    *) fail "a lease did not clear the record of a fixed address: $OUT" ;;
esac

# The three windows are one disk, and one writer at a time covers all of them:
# the boot partition and the loader's gap are inside the whole drive, so a
# reflash and a kernel install can address the same sectors by different names.
# Opening writes nothing, so this costs the disk nothing.
OUT="$(machine 'sh -c "exec 3>/dev/grubcore" 2>/dev/null; echo ALONE=$?' || true)"
case "$OUT" in
    *ALONE=0*) ;;
    *) fail "the loader's gap could not be opened for writing on its own: $OUT" ;;
esac
OUT="$(machine 'sh -c "exec 3>/dev/disk0; exec 4>/dev/grubcore" 2>/dev/null; echo BOTH=$?' || true)"
case "$OUT" in
    *BOTH=0*) fail "a second raw writer was admitted while the whole disk was held" ;;
    *BOTH=*) ;;
    *) fail "the second-writer check said nothing: $OUT" ;;
esac

# The repair itself: one pipe, from a file on the host to a partition on a disk
# nothing is mounting.
OUT="$(gzip -c "$ROOTIMG" | machine "gunzip | reflash /dev/disk0 $ROOT_OFF $ROOT_BYTES" || true)"
case "$OUT" in
    *"wrote $ROOT_BYTES bytes"*) ;;
    *) fail "the repair did not report the whole length written: $OUT" ;;
esac

# Read back through the same device and hashed on the machine: the bytes on the
# platter are compared, not the bytes the writer believes it sent.
OUT="$(machine "dd if=/dev/disk0 bs=512 skip=$((ROOT_OFF / 512)) count=$((ROOT_BYTES / 512)) 2>/dev/null | sha256sum" || true)"
case "$OUT" in
    *"$ROOT_SHA"*) ;;
    *) fail "the region read back does not hash as the image sent: $OUT" ;;
esac

# A repair is not one boot, and a machine that lost power in the middle of one
# must come back to the image that was doing the repairing: the boot partition
# is healthy, so nothing falls through to partition 3, and the ordinary kernel
# cannot mount what is half written. So recovery can be the saved default too,
# and this leaves it there before stopping the machine the way a power cut
# would. The default to put back afterwards is whatever this machine had, which
# is not always `candidate`: one on a network with no DHCP server runs from the
# fixed-address entries, and restoring the wrong one would boot a kernel that
# asks for a lease nobody answers. So it is read first, the way the instructions
# say to.
machine 'bootctl env saved_entry candidate' > /dev/null 2>&1 || true
PREV="$(machine 'bootctl env saved_entry' | sed -n 's/^bootctl: saved_entry: //p' | tr -d '\r')"
case "$PREV" in
    candidate|fallback|candidate-static|fallback-static) ;;
    *) fail "the saved default could not be read back before the repair: '$PREV'" ;;
esac
OUT="$(machine 'bootctl env saved_entry recovery && bootctl env saved_entry' || true)"
case "$OUT" in
    *"saved_entry: recovery"*) ;;
    *) fail "recovery could not be left as the saved default: $OUT" ;;
esac

machine 'sync; poweroff -f' > /dev/null 2>&1 || true
sleep 5
cleanup_boot

# The boot after that power cut: recovery again, with nothing armed for one boot
# and nothing typed. Then the saved default is put back, which is what an
# administrator does when the repair is finished.
LOG="$DISK.log.latched"
: > "$STEPS"
boot_feed net
wait_count "rach2 login:" 1 200 || fail "the saved recovery default did not boot recovery"
OUT="$(machine 'test -c /dev/disk0 && echo STILL""RECOVERY' || true)"
case "$OUT" in
    *STILLRECOVERY*) ;;
    *) fail "the machine that came back was not recovery: $OUT" ;;
esac
OUT="$(machine "bootctl env saved_entry $PREV && bootctl env saved_entry" || true)"
case "$OUT" in
    *"saved_entry: $PREV"*) ;;
    *) fail "the saved default could not be put back to what it was: $OUT" ;;
esac
machine 'sync; poweroff -f' > /dev/null 2>&1 || true
sleep 5
cleanup_boot

# Boot 4: the ordinary entry again. The disk is whole.
LOG="$DISK.log.repaired"
boot_watch "rach2 login:"
grep -aq "File system... OK (KRFFS" "$LOG" || \
    fail "the repaired machine did not mount its root"

# A recovery payload that will not load must not stop the machine. GRUB does not
# put `root` back when an entry fails, and the recovery entries set it to
# partition 3, so a fallback that assumed the root it started with would look
# for the ordinary kernel there and fail too. Every entry names its own root for
# that reason, and this is the proof: partition 3 is destroyed on a copy, the
# recovery entry is armed, and the machine must fall back to a kernel on
# partition 1 and mount its root from the disk.
cp "$WORK" "$WORK.save"
python3 -c "
import struct
d = open('$WORK', 'rb').read(512)
e = d[446 + 32:446 + 48]
start, count = struct.unpack_from('<II', e, 8)
f = open('$WORK', 'r+b')
f.seek(start * 512)
f.write(b'\x00' * (1024 * 1024))
f.close()
"
python3 -c "
d = open('$WORK', 'rb').read()
i = d.find(b'# GRUB Environment Block')
block = (b'# GRUB Environment Block\n'
         b'# WARNING: Do not edit this file by tools other than grub-editenv!!!\n'
         b'next_entry=recovery\n').ljust(1024, b'#')
f = open('$WORK', 'r+b')
f.seek(i)
f.write(block)
f.close()
"
LOG="$DISK.log.norecovery"
boot_watch "rach2 login:"
grep -aq "(disk)" "$LOG" || \
    fail "a recovery entry that could not load did not fall back to a kernel on partition 1"
mv "$WORK.save" "$WORK"

# And the network with nothing on it. A machine that reached recovery through
# the core image's own configuration is a machine whose menu nobody chose from,
# so if that network has no DHCP server there is no entry to select and no
# operator to select it. The image waits for a lease and takes the fixed address
# when none arrives; this is that path, on a hub port with nothing else on it.
# What that machine then installs has to be armed in the entries that carry the
# fixed address, or the reboot comes back with none. Its command line asked for
# a lease, so the address it is running on is the only thing that says which
# pair it is on, and the image records that where `bootctl` reads it. The client
# that did not get a lease has to be gone as well: one that answered later would
# replace the address while the record went on naming it.
if ! python3 "$REPO/tools/harness.py" --kernel "$REPO/build/rach2-recovery/kernel" \
        --initrd "$REPO/build/images/rach2-recovery.krffs" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$WORK,format=raw,if=ide,index=0,media=disk" \
        --net "hubport,id=net0,hubid=1" \
        --append "rach2.root=ram rach2.slot=recovery rach2.ip=none" \
        --timeout 300 --steptmo 120 --ok "the fixed-address entries" \
        "expect=rach2 login:" wait=45 "send=root" "expect=Password:" \
        "send=$ROOTPASSWORD" "expect=# " \
        "send=ifconfig eth0 | grep -q 'addr:192.168.0.105' && ! pidof udhcpc > /dev/null && echo NO\"\"CLIENT" \
        "expect=NOCLIENT" "send=bootctl status | grep booted" \
        "expect=the fixed-address entries" wait=3 > /dev/null 2>&1; then
    fail "recovery on a network with no server did not take the fixed address, stop the client, and say so"
fi

# And the case partition 1 cannot answer at all. The stub, the menu and every
# entry live there, so a damaged boot partition takes every way in with it; what
# is left is the core image in the gap, which has two lines of its own. The
# first opens partition 1's menu and the second opens partition 3's, and the
# early parser those run through has no `if`, which is why they are an ordered
# pair, not a test. Nothing on partition 1 can boot now, so a machine
# that reaches a login has come through the second line.
python3 -c "
import struct
d = open('$WORK', 'rb').read(512)
start, count = struct.unpack_from('<II', d, 446 + 8)
f = open('$WORK', 'r+b')
f.seek(start * 512)
f.write(b'\x00' * (2 * 1024 * 1024))
f.close()
"
LOG="$DISK.log.p1dead"
boot_watch "rach2 login:"
grep -aq "Modules... OK (1 found)" "$LOG" || \
    fail "the boot with partition 1 destroyed did not load a RAM image, so it was not recovery"

echo "drill-test: the everyday kernel refused to write its disk and wrote nothing, a destroyed root stopped the boot, recovery came up on partition 3 and answered SSH, the whole root arrived through one pipe and hashed identically on the platter, and the machine booted from it, a recovery entry that would not load fell back to a kernel on partition 1, a machine stopped with recovery saved as its default came back to recovery, and a disk whose boot partition was destroyed still reached recovery through the core image's own configuration: OK"
echo "drill-test: PASS"
