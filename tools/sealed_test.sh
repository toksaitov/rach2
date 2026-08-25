#!/usr/bin/env sh

#
# sealed_test.sh: the disk that goes into the laptop, operated the way it will
# be operated once it is in there.
#
# Every other harness proves one mechanism on a disk built for it. This one
# proves the finished article: the image `make rach2-sealed-disk` writes, on the
# controller it will use, with nothing typed at the console after the machine is
# switched on. Everything it does, it does over SSH, because after the screws go
# in that is the only way in.
#
# Two runs of one artifact, on separate copies.
#
# The fault run walks the whole life of the machine: it comes up and answers a
# key login; a new kernel arrives over the same connection, is installed as a
# one-boot trial, proves itself by a marker in its own boot, and is committed; a
# candidate destroyed from the host is caught by the fallback and repaired from
# the machine that fell back; a menu is staged and then truncated from the host,
# which the stub must survive; the boot loader is replaced with a marked core
# and put back; and finally the root is destroyed with the machine stopped and
# rewritten from recovery through an ssh pipe, after which the machine boots
# from what arrived.
#
# The control run is the same artifact with no faults injected and no deliberate
# entry into recovery, and it asserts that no fallback and no recovery token
# appears anywhere. That absence is the whole point of the second run: a run
# that entered recovery on purpose could never assert it.
#
# Usage: sealed_test.sh <sealed-disk> <root password> [ssh port]
#

set -e

DISK="$1"
ROOTPASSWORD="$2"
PORT="${3:-2297}"

if [ -z "$DISK" ] || [ -z "$ROOTPASSWORD" ]; then
    echo "usage: sealed_test.sh <sealed-disk> <root password> [ssh port]" >&2
    exit 2
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEADLINE=3600
START="$(date +%s)"
PHASE_AT="$START"

WORK="$DISK.work"
KNOWN="$DISK.known"
ROOTIMG="$DISK.root.img"
MARKED="$DISK.marked.kernel"
CORE="$DISK.core.img"
LOG=""
QPID=""

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
    rm -f "$WORK" "$KNOWN" "$ROOTIMG" "$MARKED" "$CORE" "$DISK".log*
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fail() {
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        cp "$LOG" "$DISK.fail.log" 2>/dev/null || true
        echo "sealed-test: the failing boot's console log: $DISK.fail.log" >&2
    fi
    cleanup_boot
    echo "sealed-test: FAIL: $1" >&2
    exit 1
}

# Each phase says how long it took, because the budget for this rehearsal is a
# number in the plan and a harness that grew past it should say which part grew.
phase() {
    now="$(date +%s)"
    if [ -n "${PHASE_NAME:-}" ]; then
        echo "sealed-test: $PHASE_NAME: $((now - PHASE_AT))s"
    fi
    PHASE_NAME="$1"
    PHASE_AT="$now"
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

# A cold boot of $WORK: a new QEMU, a new log, the SSH port forwarded, and
# nothing typed at the console. The machine is on the network the sealed image
# is built for, so the address it asks for is answered the way the building's
# router will answer it.
start_qemu() {
    LOG="$DISK.log.$1"
    rm -f "$LOG" "$KNOWN"
    qemu-system-i386 -cpu max -display none -rtc base=utc \
        -drive "file=$WORK,format=raw,if=none,id=drv0" \
        -device ich9-ahci,id=ahci0 -device ide-hd,drive=drv0,bus=ahci0.0 \
        -netdev "user,id=n0,net=192.168.0.0/24,host=192.168.0.2,hostfwd=tcp::$PORT-:22" \
        -device rtl8139,netdev=n0 \
        -serial stdio < /dev/null > "$LOG" 2>&1 &
    QPID=$!
}

# A boot that is expected to reach a login and answer SSH.
cold_boot() {
    start_qemu "$1"
    wait_count "rach2 login:" 1 180 || fail "the machine never reached a login prompt"
    # The server is started by init after the network, so what is waited for is
    # the port answering, not a line on the console.
    i=0
    while [ "$i" -lt 60 ]; do
        if machine 'echo up' > /dev/null 2>&1; then
            return 0
        fi
        sleep 2
        i=$((i + 2))
    done
    fail "the machine never answered SSH"
}

# A boot that is expected to stop somewhere: it waits for one line and looks at
# nothing else. Used where the machine is meant to fail, since a boot that
# reaches no login has no SSH to ask.
cold_watch() {
    start_qemu "$1"
    wait_count "$2" 1 150 || fail "the boot never reached: $2"
}

# ssh in as root by key, with nothing of the runner's own offered. Every call is
# bounded: a session that hangs would otherwise hold the whole rehearsal, and a
# machine that stopped answering mid-command is a failure to report rather than
# a reason to wait.
machine() {
    $SSHTMO ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o ConnectTimeout=20 -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$REPO/krffs/rach2_demo_ed25519" -p "$PORT" root@localhost "$1" 2>&1
}

# Stopping the machine is a command it does not answer: the connection dies with
# it, so the call is bounded tightly and its result ignored, and what is waited
# for is the process going.
stop_machine() {
    $QUICKTMO ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -i "$REPO/krffs/rach2_demo_ed25519" \
        -p "$PORT" root@localhost 'sync; poweroff -f' > /dev/null 2>&1 || true
    i=0
    while [ "$i" -lt 30 ] && kill -0 "$QPID" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
    done
    cleanup_boot
}

# The machine restarts inside the same QEMU; what says it came back is another
# login prompt, counted, not matched, since the log has every boot.
reboot_machine() {
    want="$1"
    # Bounded and its result ignored, for the reason stopping is: the connection
    # dies with the machine, so there is no answer to wait for.
    $QUICKTMO ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o ConnectTimeout=10 -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -i "$REPO/krffs/rach2_demo_ed25519" \
        -p "$PORT" root@localhost 'sync; reboot -f' > /dev/null 2>&1 || true
    wait_count "rach2 login:" "$want" 180 || fail "the machine did not come back (boot $want)"
    i=0
    while [ "$i" -lt 60 ]; do
        if machine 'echo up' > /dev/null 2>&1; then
            return 0
        fi
        sleep 2
        i=$((i + 2))
    done
    fail "the machine came back but never answered SSH (boot $want)"
}

# A bound on every ssh, where the host has a timeout(1) to give.
SSHTMO=""
QUICKTMO=""
for t0 in timeout gtimeout; do
    if command -v "$t0" >/dev/null 2>&1; then
        SSHTMO="$t0 180"
        QUICKTMO="$t0 20"
        break
    fi
done
if [ -z "$SSHTMO" ]; then
    # Not a warning: without it a session that stalls after connecting never
    # returns, and the run's own deadline is only looked at between boots. A
    # rehearsal that can hang for ever is not a proof of anything.
    echo "sealed-test: timeout(1) or gtimeout(1) is required (brew install coreutils)" >&2
    exit 2
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

# ---------------------------------------------------------------- the artifact

phase "prepare"

if [ ! -f "$DISK" ]; then
    fail "no sealed disk at $DISK"
fi
DISK_SHA="$(sha256_of "$DISK")"
rm -f "$WORK"
cp "$DISK" "$WORK"
if [ "$(sha256_of "$WORK")" != "$DISK_SHA" ]; then
    fail "the copy does not hash as the artifact"
fi
echo "sealed-test: artifact $DISK_SHA ($(wc -c < "$DISK") bytes)"

# The partitions, read off the artifact's own table. Everything this run writes
# or destroys is placed by these numbers and not by the sizes the build was
# given, because what is being rehearsed is the disk as it is.
# The root is found the way the kernel finds it: by reading each entry's first
# sector and taking the first that has a KRFFS root node, not by trusting an
# entry number or a type byte. What is destroyed below has to be the partition
# the machine actually mounts.
EXTENTS="$(python3 -c "
import struct, sys
f = open('$WORK', 'rb')
table = f.read(512)
boot = 0
root = None
for i in range(4):
    e = table[446 + 16 * i:446 + 16 * i + 16]
    start, count = struct.unpack_from('<II', e, 8)
    if start == 0 or count == 0:
        continue
    if boot == 0:
        boot = start
    f.seek(start * 512)
    head = f.read(512)
    magic, kind = struct.unpack_from('<HB', head, 0)
    if magic == 0x4B52 and kind == 0 and root is None:
        root = (start, count)
if root is None:
    sys.exit('sealed-test: no partition on the artifact has a KRFFS root')
print('%d %d %d' % (boot, root[0], root[1]))
")"
BOOT_LBA="$(echo "$EXTENTS" | cut -d' ' -f1)"
ROOT_LBA="$(echo "$EXTENTS" | cut -d' ' -f2)"
ROOT_SECTORS="$(echo "$EXTENTS" | cut -d' ' -f3)"
if [ -z "$ROOT_LBA" ] || [ "$ROOT_LBA" -eq 0 ]; then
    fail "the artifact's partition table names no root"
fi
ROOT_OFF=$((ROOT_LBA * 512))
ROOT_BYTES=$((ROOT_SECTORS * 512))

# The root as it shipped, for the repair at the end, and its hash.
dd if="$WORK" of="$ROOTIMG" bs=512 skip="$ROOT_LBA" count="$ROOT_SECTORS" status=none
ROOT_SHA="$(sha256_of "$ROOTIMG")"

# A kernel that can be told from the one on the disk. It is the same build with
# its banner rewritten in place, which is the smallest edit that makes a boot
# say which kernel booted it; the structural checker still accepts it, and a
# machine that installs it prints the marker in its own boot and nowhere else.
python3 -c "
d = open('$REPO/build/rach2/kernel', 'rb').read()
old = b'======= Rach2 Booting... ======='
new = b'======= Rach2 SEALEDMARK ======='
if d.count(old) == 0:
    raise SystemExit('sealed-test: the kernel has no banner to mark')
open('$MARKED', 'wb').write(d.replace(old, new))
"
python3 "$REPO/tools/inspect/check_kernel.py" "$MARKED" > /dev/null || \
    fail "the marked kernel does not pass the structural checker"

# ------------------------------------------------------------ the fault run

phase "first cold boot"
cold_boot fault

OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the first boot did not come from the candidate slot: $OUT" ;;
esac
# Where the candidate slot is, from the machine's own report of it, so what is
# destroyed later is the slot the machine would boot, not a number
# computed here.
CAND_SECTOR="$(printf '%s' "$OUT" | sed -n 's/^bootctl: candidate: first sector \([0-9]*\),.*/\1/p')"
if [ -z "$CAND_SECTOR" ]; then
    fail "the machine never reported the candidate extent: $OUT"
fi
case "$OUT" in
    *"next_entry: absent"*) ;;
    *) fail "a freshly built disk has a trial armed: $OUT" ;;
esac
if grep -aq "Falling back to" "$LOG"; then
    fail "the first boot of a good disk took the fallback"
fi
# The address is the building's to give, and the machine asked for it: the disk
# has rach2.ip=none and init runs the client.
grep -aq "lease of" "$LOG" || fail "the machine did not take a lease"

phase "kernel over ssh, trial, commit"
# The update arrives the way every update will: down the same connection.
machine 'cat > /tmp/kernel' < "$MARKED" > /dev/null 2>&1 || fail "the kernel did not arrive"
OUT="$(machine "test -s /tmp/kernel && bootctl kernel /tmp/kernel" || true)"
case "$OUT" in
    *"installed:"*"candidate slot"*) ;;
    *) fail "the install did not report success: $OUT" ;;
esac
if OUT="$(machine 'bootctl commit')"; then
    fail "a commit while the trial was still armed reported success: $OUT"
fi
case "$OUT" in
    *"has not booted"*) ;;
    *) fail "the refused commit did not say why: $OUT" ;;
esac
# And it changed nothing: the saved default is still the fallback the install
# moved it to, and the trial is still armed.
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"saved_entry: fallback"*) ;;
    *) fail "the refused commit moved the saved default: $OUT" ;;
esac
case "$OUT" in
    *"next_entry: candidate"*) ;;
    *) fail "the refused commit disarmed the trial: $OUT" ;;
esac

reboot_machine 2
grep -aq "Rach2 SEALEDMARK" "$LOG" || fail "the trial boot did not run the installed kernel"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the trial boot did not come from the candidate slot: $OUT" ;;
esac
OUT="$(machine 'bootctl commit' || true)"
case "$OUT" in
    *"committed: the candidate is the saved default"*) ;;
    *) fail "the commit after a trial boot did not report success: $OUT" ;;
esac
stop_machine

phase "a destroyed candidate, the fallback, and the repair"
# The candidate slot's first sector, destroyed with the machine stopped. GRUB
# loads a kernel and this one no longer is one, so the fallback number takes the
# other slot: that is the whole reason two slots exist.
python3 -c "
f = open('$WORK', 'r+b')
f.seek(($BOOT_LBA + $CAND_SECTOR) * 512)
f.write(b'\x00' * 512)
f.close()
"

cold_boot fallback
grep -aq "Falling back to" "$LOG" || fail "a destroyed candidate did not take the fallback"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: fallback"*) ;;
    *) fail "the machine did not come up on the fallback: $OUT" ;;
esac

# The repair is the same install, from the machine that fell back.
machine 'cat > /tmp/kernel' < "$MARKED" > /dev/null 2>&1 || fail "the repair kernel did not arrive"
OUT="$(machine 'bootctl kernel /tmp/kernel' || true)"
case "$OUT" in
    *"installed:"*"candidate slot"*) ;;
    *) fail "the repair install did not report success: $OUT" ;;
esac
reboot_machine 2
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the repaired candidate did not boot: $OUT" ;;
esac
OUT="$(machine 'bootctl commit' || true)"
case "$OUT" in
    *"committed: the candidate is the saved default"*) ;;
    *) fail "the repaired candidate could not be committed: $OUT" ;;
esac
stop_machine

phase "a staged menu, and a torn one"
cold_boot menu
machine "printf 'menuentry sealedone --id sealedone {\\nmultiboot /boot/fallback rach2.root=disk rach2.menu=sealedone\\nboot\\n}\\n' > /tmp/m.cfg" > /dev/null 2>&1 || true
OUT="$(machine 'bootctl config /tmp/m.cfg' || true)"
case "$OUT" in
    *"staged:"*"menu in a"*) ;;
    *) fail "the menu did not stage: $OUT" ;;
esac
reboot_machine 2
grep -aq "sealedone" "$LOG" || fail "the staged entry never reached the menu"
stop_machine

# Truncated from the host, mid-entry: the file keeps its size, so nothing about
# the volume changes, and the stub must survive a menu it cannot parse.
python3 -c "
d = open('$WORK', 'rb').read()
i = d.find(b'menuentry sealedone --id sealedone')
if i < 0:
    raise SystemExit('sealed-test: the staged menu is not on the disk')
head = b'menuentry sealedone --id sealedone {\n'
f = open('$WORK', 'r+b')
f.seek(i)
f.write(head + b'#' * (4096 - len(head)))
f.close()
"
cold_boot torn
grep -aqi "error" "$LOG" || fail "a truncated menu produced no loader error"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "a truncated menu stopped the machine booting its candidate: $OUT" ;;
esac
stop_machine

phase "the boot loader, replaced and put back"
# A core image with a marker in it, installed into the tail that is not in use.
# The marker prints before anything on a partition, so what it proves is which
# image the machine is running, not which bytes are at rest.
sh "$REPO/tools/image/mkgrubcore.sh" SEALEDCORE1 "$CORE" > /dev/null 2>&1 || \
    fail "the marked core image could not be built"
cold_boot core
machine 'cat > /tmp/core.img' < "$CORE" > /dev/null 2>&1 || fail "the core image did not arrive"
OUT="$(machine 'bootctl grub /tmp/core.img' || true)"
case "$OUT" in
    *"installed:"*"boot loader"*) ;;
    *) fail "the boot loader install did not report success: $OUT" ;;
esac
FIRST_TAIL="$(printf '%s' "$OUT" | sed -n 's/.*tail at LBA \([0-9]*\).*/\1/p')"
if [ -z "$FIRST_TAIL" ]; then
    fail "the install did not say which tail it wrote: $OUT"
fi
reboot_machine 2
grep -aq "GRUBCORE=SEALEDCORE1" "$LOG" || fail "the machine did not boot the core it installed"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the machine did not boot its candidate through the new loader: $OUT" ;;
esac

# And back again. The gap has two tails and an install writes the one that is
# not running, so a second install has to land in the other one: a defect in
# that direction alone would leave a machine whose next loader update makes GRUB
# unreachable, and recovery with it.
sh "$REPO/tools/image/mkgrubcore.sh" SEALEDCORE2 "$CORE" > /dev/null 2>&1 || \
    fail "the second core image could not be built"
machine 'cat > /tmp/core.img' < "$CORE" > /dev/null 2>&1 || fail "the second core did not arrive"
OUT="$(machine 'bootctl grub /tmp/core.img' || true)"
case "$OUT" in
    *"installed:"*"boot loader"*) ;;
    *) fail "the second boot loader install did not report success: $OUT" ;;
esac
SECOND_TAIL="$(printf '%s' "$OUT" | sed -n 's/.*tail at LBA \([0-9]*\).*/\1/p')"
if [ -z "$SECOND_TAIL" ] || [ "$SECOND_TAIL" = "$FIRST_TAIL" ]; then
    fail "the second install wrote the same tail as the first ($FIRST_TAIL and $SECOND_TAIL)"
fi
reboot_machine 3
grep -aq "GRUBCORE=SEALEDCORE2" "$LOG" || fail "the machine did not boot the second core"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the machine did not boot its candidate through the second loader: $OUT" ;;
esac
stop_machine

phase "the root destroyed, and rewritten from recovery"
# The root's first sector, destroyed with the machine stopped, and the recovery
# entry armed the way an operator arms it from the console: this is the one
# damage that leaves the machine with no way in of its own.
python3 -c "
f = open('$WORK', 'r+b')
f.seek($ROOT_OFF)
f.write(b'\x00' * 512)
f.close()
"
# The damage is proved before it is repaired: the ordinary entry must fail to
# mount and offer nothing, which is the state an operator would find the machine
# in. Booting straight into recovery would prove the repair without ever showing
# that anything was broken.
cold_watch broken "no KRFFS partition on the disk"
if grep -aq "rach2 login:" "$LOG"; then
    fail "a machine whose root is destroyed still reached a login"
fi
cleanup_boot

# Armed the way `grub-editenv` arms it: one key set and every other line kept.
# Writing the block from nothing would drop the saved default with it, and the
# machine would come back on the fallback for a reason that had nothing to do
# with the repair.
python3 -c "
d = open('$WORK', 'rb').read()
i = d.find(b'# GRUB Environment Block')
if i < 0:
    raise SystemExit('sealed-test: the disk has no environment block')
old = d[i:i + 1024]
keep = []
for line in old.split(b'\n'):
    line = line.rstrip(b'#')
    if not line or line.startswith(b'#') or line.startswith(b'next_entry='):
        continue
    keep.append(line)
head = (b'# GRUB Environment Block\n'
        b'# WARNING: Do not edit this file by tools other than grub-editenv!!!\n')
block = head + b'\n'.join(keep + [b'next_entry=recovery', b''])
f = open('$WORK', 'r+b')
f.seek(i)
f.write(block.ljust(1024, b'#'))
f.close()
"

cold_boot recovery
grep -aq "Modules... OK" "$LOG" || fail "the armed entry did not load the recovery image"
OUT="$(machine 'test -c /dev/disk0 && echo RECOVERY""HERE' || true)"
case "$OUT" in
    *RECOVERYHERE*) ;;
    *) fail "the recovery entry did not come up with the whole-disk device: $OUT" ;;
esac
OUT="$(gzip -c "$ROOTIMG" | machine "gunzip | reflash /dev/disk0 $ROOT_OFF $ROOT_BYTES" || true)"
case "$OUT" in
    *"wrote $ROOT_BYTES bytes"*) ;;
    *) fail "the repair did not report the whole root written: $OUT" ;;
esac
OUT="$(machine "dd if=/dev/disk0 bs=512 skip=$ROOT_LBA count=$ROOT_SECTORS 2>/dev/null | sha256sum" || true)"
case "$OUT" in
    *"$ROOT_SHA"*) ;;
    *) fail "the root read back does not hash as the image sent: $OUT" ;;
esac
stop_machine

phase "the last cold boot"
cold_boot final
if grep -aq "Falling back to" "$LOG"; then
    fail "the machine ended the run taking a fallback"
fi
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the repaired machine did not boot its candidate: $OUT" ;;
esac
grep -aq "lease of" "$LOG" || fail "the repaired machine did not take a lease"
stop_machine

# ---------------------------------------------------------- the control run

phase "the control run"
# A fresh copy, no faults, and no deliberate entry into recovery. What it
# asserts is an absence, which is why it cannot be the same run: a machine that
# was sent to recovery on purpose can never say that recovery was not reached.
rm -f "$WORK"
cp "$DISK" "$WORK"
cold_boot control
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the control run did not boot its candidate: $OUT" ;;
esac
if grep -aq "Falling back to" "$LOG"; then
    fail "the control run took a fallback with nothing injected"
fi
# What says a boot was recovery is that it loaded a RAM image: the ordinary
# entries mount their root from the disk and load no module. The word itself is
# no use, since the menu draws every entry's title on the console.
if grep -aq "Modules... OK" "$LOG"; then
    fail "the control run reached recovery with nothing injected"
fi
grep -aq "lease of" "$LOG" || fail "the control run took no lease"
# And nothing about it left a trial armed or a default moved.
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"next_entry: absent"*) ;;
    *) fail "the control run left a trial armed: $OUT" ;;
esac
stop_machine

phase ""
echo "sealed-test: the sealed artifact came up on AHCI and answered a key login; a kernel sent down that connection was installed, tried and committed; a destroyed candidate was caught by the fallback and repaired from it; a staged menu reached the machine and a torn one left it booting; the boot loader was replaced with a marked core and then with another in the other tail, and the machine booted each; the root was destroyed and rewritten from recovery through one pipe, hashing identically on the platter; and a control copy did all of the ordinary work with no fallback and no recovery anywhere: OK"
echo "sealed-test: PASS"
