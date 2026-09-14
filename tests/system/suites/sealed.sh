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
# The fault run walks the whole life of the machine: it comes up on the ext2
# root the candidate entries carry and answers a key login; a new kernel
# arrives over the same connection, is installed as a one-boot trial, proves
# itself by a marker in its own boot, and is committed; a candidate destroyed
# from the host is caught by the fallback, which boots the same root on the
# previous kernel, and repaired from the machine that fell back; the root is
# made missing, faulty and dirty in turn under an armed trial and each lands in
# the recovery system, after which recovery repairs it and the candidate is
# committed again; a menu is staged and then truncated from the host, which the stub
# must survive; the boot loader is replaced with a marked core and put back;
# and finally the root is destroyed with the machine stopped and rewritten from
# recovery through an ssh pipe, after which the machine boots from what
# arrived.
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

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"
DEADLINE=$(scaled 3600)
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
    rm -f "$WORK" "$WORK.sb" "$KNOWN" "$ROOTIMG" "$MARKED" "$CORE" "$DISK.ext2.img" "$DISK".log*
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
    wait_count "rach2 login:" 1 "$(scaled 180)" || fail "the machine never reached a login prompt"
    # The server is started by init after the network, so what is waited for is
    # the port answering, not a line on the console.
    i=0
    while [ "$i" -lt "$(scaled 60)" ]; do
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
    wait_count "$2" 1 "$(scaled 150)" || fail "the boot never reached: $2"
}

# ssh in as root by key, with nothing of the runner's own offered. Every call is
# bounded: a session that hangs would otherwise hold the whole rehearsal, and a
# machine that stopped answering mid-command is a failure to report rather than
# a reason to wait.
machine() {
    $SSHTMO ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 20)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -o PasswordAuthentication=no \
        -i "$REPO/krffs/rach2_demo_ed25519" -p "$PORT" root@localhost "$1" 2>&1
}

# Stopping the machine is a command it does not answer: the connection dies with
# it, so the call is bounded tightly and its result ignored, and what is waited
# for is the process going.
stop_machine() {
    $QUICKTMO ssh -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$KNOWN" \
        -o "ConnectTimeout=$(scaled 10)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -i "$REPO/krffs/rach2_demo_ed25519" \
        -p "$PORT" root@localhost 'sync; poweroff -f' > /dev/null 2>&1 || true
    i=0
    while [ "$i" -lt "$(scaled 30)" ] && kill -0 "$QPID" 2>/dev/null; do
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
        -o "ConnectTimeout=$(scaled 10)" -o BatchMode=yes -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey -i "$REPO/krffs/rach2_demo_ed25519" \
        -p "$PORT" root@localhost 'sync; reboot -f' > /dev/null 2>&1 || true
    wait_count "rach2 login:" "$want" "$(scaled 180)" || fail "the machine did not come back (boot $want)"
    i=0
    while [ "$i" -lt "$(scaled 60)" ]; do
        if machine 'echo up' > /dev/null 2>&1; then
            return 0
        fi
        sleep 2
        i=$((i + 2))
    done
    fail "the machine came back but never answered SSH (boot $want)"
}

# A bound on every ssh, through the project deadline tool: without it a
# session that stalls after connecting never returns, and the run's own
# deadline is only looked at between boots. A rehearsal that can hang for
# ever is not a proof of anything.
DEADLINE_TOOL="$REPO/build/tests/system/deadline"
if [ ! -x "$DEADLINE_TOOL" ]; then
    echo "sealed-test: $DEADLINE_TOOL is not built; run make first" >&2
    exit 2
fi
SSHTMO="$DEADLINE_TOOL $(scaled 180)"
QUICKTMO="$DEADLINE_TOOL $(scaled 20)"

sha256_of() {
    if [ ! -x "$REPO/build/tools/verify/sha256" ]; then
        mkdir -p "$REPO/build/tools/verify"
        cc -O2 -o "$REPO/build/tools/verify/sha256" \
            "$REPO/tools/verify/sha256.c" || return 1
    fi
    "$REPO/build/tools/verify/sha256" "$1" | awk '{print $1}'
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
# The root is found the way the kernel finds it, by its superblock magic and
# not by an entry number or a type byte: the first partition whose superblock,
# at byte 1024, has 0xEF53 at byte 56. What is destroyed below has to be the
# partition the machine actually mounts. The first partition is the boot one.
#
# There were two roots here until 2026-09-14, a KRFFS one the fallback booted
# and this ext2 one, each discovered separately. The disk has one.
BOOT_LBA="$("$REPO/build/tools/image/mkmbr" --list "$WORK" |
    awk 'NR == 1 { print $3 / 512; exit }')"
EXTENTS="$("$REPO/build/tools/image/mkmbr" --list "$WORK" |
    while read -r idx type start size; do
        [ -n "$idx" ] || continue
        magic="$(dd if="$WORK" bs=512 skip=$((start / 512 + 2)) count=1 \
            status=none | od -An -tx1 -j 56 -N 2 | tr -d ' ')"
        if [ "$magic" = "53ef" ]; then
            printf '%s %s\n' $((start / 512)) $((size / 512))
            break
        fi
    done)"
[ -n "$EXTENTS" ] || fail "no partition on the artifact has an ext2 root"
ROOT_LBA="$(echo "$EXTENTS" | cut -d' ' -f1)"
ROOT_SECTORS="$(echo "$EXTENTS" | cut -d' ' -f2)"
EXT2_LBA="$ROOT_LBA"
EXT2_OFF=$((EXT2_LBA * 512))
if [ -z "$ROOT_LBA" ] || [ "$ROOT_LBA" -eq 0 ]; then
    fail "the artifact's partition table names no root"
fi
ROOT_OFF=$((ROOT_LBA * 512))
ROOT_BYTES=$((ROOT_SECTORS * 512))

# The root as it shipped, for the repair at the end, and its hash.
dd if="$WORK" of="$ROOTIMG" bs=512 skip="$ROOT_LBA" count="$ROOT_SECTORS" status=none
ROOT_SHA="$(sha256_of "$ROOTIMG")"

# A kernel that can be told from the one on the disk. It is the same build with
# the text of its first boot record rewritten in place, which is the smallest
# edit that makes a boot say which kernel booted it; the mark is the same
# length as what it replaces, so nothing in the artifact moves. The structural
# checker still accepts it, and a machine that installs it prints the marker in
# its own boot and nowhere else.
cp "$REPO/build/rach2/kernel" "$MARKED"
"$REPO/build/tools/inspect/binfind" --replace \
    'Rach2 Booting' 'Rach2 SEALED!' \
    "$MARKED" || fail "the kernel has no banner to mark"
"$REPO/build/tools/inspect/elfcheck" --kernel "$MARKED" > /dev/null || \
    fail "the marked kernel does not pass the structural checker"

# ------------------------------------------------------------ the fault run

phase "first cold boot"
cold_boot fault

OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the first boot did not come from the candidate slot: $OUT" ;;
esac
# The candidate's root is the ext2 partition, by its own command line and by
# the mount table; the fallback boots the same one.
ROOTS="$(machine 'cat /proc/cmdline; cat /proc/mounts' || true)"
case "$ROOTS" in
    *"rach2.root=MBRID="*",4 rach2.rootfstype=ext2"*) ;;
    *) fail "the candidate's command line does not name the ext2 root: $ROOTS" ;;
esac
case "$ROOTS" in
    *"/dev/disk0p4 / ext2 rw,relatime"*) ;;
    *) fail "the candidate did not mount the ext2 root writable: $ROOTS" ;;
esac
if grep -aq "ext2: check" "$LOG"; then
    fail "a freshly built ext2 root was checked at its first boot"
fi
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
grep -aq "Rach2 SEALED!" "$LOG" || fail "the trial boot did not run the installed kernel"
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
# The committed candidate becomes the fallback kernel: the slot copied into
# the fallback file and read back, then tried once through the fallback
# entry, which the marked kernel's token proves ran from that file.
OUT="$(machine 'bootctl fallback' || true)"
case "$OUT" in
    *"fallback: "*"written from the committed candidate and read back"*) ;;
    *) fail "the committed candidate was not written as the fallback: $OUT" ;;
esac
OUT="$(machine 'bootctl trial fallback' || true)"
case "$OUT" in
    *"armed: the next boot tries fallback"*) ;;
    *) fail "the fallback trial was not armed: $OUT" ;;
esac
reboot_machine 3
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: fallback"*) ;;
    *) fail "the trial boot did not come from the fallback: $OUT" ;;
esac
[ "$(grep -ac "Rach2 SEALED!" "$LOG")" -ge 2 ] ||
    fail "the fallback file does not carry the committed kernel"
stop_machine

phase "a destroyed candidate, the fallback, and the repair"
# The candidate slot's first sector, destroyed with the machine stopped. GRUB
# loads a kernel and this one no longer is one, so the fallback number takes the
# other slot: that is the whole reason two slots exist.
dd if=/dev/zero of="$WORK" bs=512 seek=$((BOOT_LBA + CAND_SECTOR)) \
    count=1 conv=notrunc status=none

cold_boot fallback
grep -aq "Falling back to" "$LOG" || fail "a destroyed candidate did not take the fallback"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: fallback"*) ;;
    *) fail "the machine did not come up on the fallback: $OUT" ;;
esac
# The fallback boots the machine's own root, which is what makes it useful: a
# kernel that will not load costs the previous kernel and nothing else. It
# booted a KRFFS root of its own until 2026-09-14, so falling back also meant
# losing the person's files and landing in the demonstration accounts.
ROOTS="$(machine 'cat /proc/cmdline; cat /proc/mounts' || true)"
case "$ROOTS" in
    *"rach2.root=MBRID="*",3 rach2.rootfstype=ext2"*) ;;
    *) fail "the fallback's command line does not name the installed root: $ROOTS" ;;
esac
case "$ROOTS" in
    *"/dev/disk0p3 / ext2"*) ;;
    *) fail "the fallback did not mount the installed root: $ROOTS" ;;
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

phase "the root missing, faulty and dirty, each landing in recovery"
# The trial is what makes a root that does not come up recoverable without a
# person, and what it falls to changed on 2026-09-14. Both kernel entries now
# boot the same root, so a damaged root is damaged for both of them and GRUB
# cannot save the machine: its automatic fallback fires when a kernel image
# will not load, not when a kernel loads and finds no file system. The saved
# default is therefore the recovery system for the whole of a root write, which
# is what `rach2-install --update` now leaves behind, and the candidate is
# armed for one boot on top of it.
#
# So each damage is two boots: the armed one, which must fail to mount and
# reach no login, and the one after it, which must be recovery with nothing
# still armed. Three damages: the superblock gone, so no ext2 partition is
# found; a reached inode the bitmap calls free, a fault, so the root mounts
# read-only; and the valid bit alone clear, so the root is checked and mounts.
# None is committed, since nothing commits a candidate but a person. The
# superblock is kept from the host copy and put back before the next damage.
dd if="$WORK" of="$WORK.sb" bs=512 skip=$((EXT2_LBA + 2)) count=2 status=none
arm_candidate_trial() {
    cold_boot "$1"
    OUT="$(machine 'bootctl env saved_entry recovery && bootctl trial candidate && bootctl status' || true)"
    case "$OUT" in
        *"next_entry: candidate"*) ;;
        *) fail "the candidate trial could not be armed: $OUT" ;;
    esac
    stop_machine
}
# The boot after a damaged one: the saved default, which is recovery, with the
# trial spent. Recovery runs from RAM, so it comes up whatever the root is in.
landed_in_recovery() {
    cold_boot "$1"
    OUT="$(machine 'bootctl status; cat /proc/mounts' || true)"
    case "$OUT" in
        *"booted: recovery"*) ;;
        *) fail "the boot after a $2 root was not recovery: $OUT" ;;
    esac
    case "$OUT" in
        *"next_entry: absent"*|*"next_entry: empty"*) ;;
        *) fail "the trial was still armed after a $2 root: $OUT" ;;
    esac
    stop_machine
}
# Missing: the superblock zeroed. The kernel finds no ext2 partition, mounts
# nothing and halts, which is the state the trial exists for.
arm_candidate_trial arm-missing
dd if=/dev/zero of="$WORK" bs=512 seek=$((EXT2_LBA + 2)) count=2 conv=notrunc status=none
cold_watch missing "root: MBRID=1acef00d,3: /dev/disk0p3 has no ext2 file system this kernel mounts"
if grep -aq "rach2 login:" "$LOG"; then
    fail "a candidate whose ext2 root is missing still reached a login"
fi
cleanup_boot
landed_in_recovery after-missing missing
dd if="$WORK.sb" of="$WORK" bs=512 seek=$((EXT2_LBA + 2)) count=2 conv=notrunc status=none
# Faulty: a reached inode's bit cleared, with the valid bit clear so that the
# check runs. The root mounts read-only with the fault named; the machine
# may reach a login on it, and it is not committed.
. "$REPO/tools/setup/e2fsprogs_path.sh"
EXT2_SECTORS="$("$REPO/build/tools/image/mkmbr" --list "$WORK" |
    awk -v lba="$EXT2_OFF" '$3 == lba { print $4 / 512; exit }')"
[ -n "$EXT2_SECTORS" ] || fail "the ext2 partition's size could not be read from the table"
EXT2IMG="$DISK.ext2.img"
dd if="$WORK" of="$EXT2IMG" bs=512 skip="$EXT2_LBA" count="$EXT2_SECTORS" status=none
debugfs -w -R 'freei /etc/hostname' "$EXT2IMG" > /dev/null 2>&1
debugfs -w -R 'ssv state 0' "$EXT2IMG" > /dev/null 2>&1
arm_candidate_trial arm-faulty
dd if="$EXT2IMG" of="$WORK" bs=512 seek="$EXT2_LBA" conv=notrunc status=none
cold_watch faulty "ext2: mounted read-only: the check found a fault"
grep -aq "ext2: check: inode [0-9]*: a reached inode the bitmap calls free" "$LOG" || \
    fail "the faulty root's fault was not named"
cleanup_boot
landed_in_recovery after-faulty faulty
# Dirty: the valid bit clear over a consistent volume. The check passes and
# the root mounts writable; still nothing commits it.
dd if="$WORK" of="$EXT2IMG" bs=512 skip="$EXT2_LBA" count="$EXT2_SECTORS" status=none
# Under set -e the status is read inside the test: 0 clean, 1 or 2 repaired.
if e2fsck -fp "$EXT2IMG" > /dev/null 2>&1; then
    fsck_status=0
else
    fsck_status=$?
fi
[ "$fsck_status" -le 2 ] || fail "e2fsck -p on the host copy exited $fsck_status"
debugfs -w -R 'ssv state 0' "$EXT2IMG" > /dev/null 2>&1
arm_candidate_trial arm-dirty
dd if="$EXT2IMG" of="$WORK" bs=512 seek="$EXT2_LBA" conv=notrunc status=none
cold_watch dirty "rach2 login:"
grep -aq "ext2: check: clean" "$LOG" || fail "the dirty root was not checked clean"
cleanup_boot
landed_in_recovery after-dirty dirty
# The repair and the return: the recovery entry whose command line includes
# `rach2.fsck` is armed, its init checks every ext2 partition, and the
# candidate, tried once more, comes up clean and is committed, which is the
# state the run continues in.
cold_boot arm-recovery
OUT="$(machine 'bootctl trial recovery-fsck && bootctl status' || true)"
case "$OUT" in
    *"next_entry: recovery-fsck"*) ;;
    *) fail "recovery could not be armed for the repair: $OUT" ;;
esac
stop_machine
cold_boot repair
grep -aq "init: e2fsck: /dev/disk0p4: status [01]" "$LOG" || \
    fail "recovery's init did not check the ext2 root"
OUT="$(machine 'bootctl trial candidate && bootctl status' || true)"
case "$OUT" in
    *"next_entry: candidate"*) ;;
    *) fail "the candidate could not be armed after the repair: $OUT" ;;
esac
stop_machine
cold_boot repaired
if grep -aq "ext2: check" "$LOG"; then
    fail "the repaired ext2 root was checked again"
fi
OUT="$(machine 'bootctl status; cat /proc/mounts' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the repaired candidate did not boot: $OUT" ;;
esac
case "$OUT" in
    *"/dev/disk0p4 / ext2 rw,relatime"*) ;;
    *) fail "the repaired candidate did not mount the ext2 root writable: $OUT" ;;
esac
OUT="$(machine 'bootctl commit' || true)"
case "$OUT" in
    *"committed: the candidate is the saved default"*) ;;
    *) fail "the repaired candidate could not be committed: $OUT" ;;
esac
stop_machine
rm -f "$WORK.sb" "$EXT2IMG"

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
MENU_AT="$("$REPO/build/tools/inspect/binfind" --offset \
    'menuentry sealedone --id sealedone' "$WORK")" || \
    fail "the staged menu is not on the disk"
printf 'menuentry sealedone --id sealedone {\n' > "$WORK.menu"
n="$(wc -c < "$WORK.menu" | tr -d ' ')"
dd if=/dev/zero bs=1 count=$((4096 - n)) 2>/dev/null | tr '\0' '#' \
    >> "$WORK.menu"
dd if="$WORK.menu" of="$WORK" bs=1 seek="$MENU_AT" conv=notrunc status=none
rm -f "$WORK.menu"
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
sh "$REPO/tools/image/mkgrubcore.sh" A "$CORE" > /dev/null 2>&1 || \
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
grep -aq "GRUBCORE=A" "$LOG" || fail "the machine did not boot the core it installed"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the machine did not boot its candidate through the new loader: $OUT" ;;
esac

# And back again. The gap has two tails and an install writes the one that is
# not running, so a second install has to land in the other one: a defect in
# that direction alone would leave a machine whose next loader update makes GRUB
# unreachable, and recovery with it.
sh "$REPO/tools/image/mkgrubcore.sh" B "$CORE" > /dev/null 2>&1 || \
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
grep -aq "GRUBCORE=B" "$LOG" || fail "the machine did not boot the second core"
OUT="$(machine 'bootctl status' || true)"
case "$OUT" in
    *"booted: candidate"*) ;;
    *) fail "the machine did not boot its candidate through the second loader: $OUT" ;;
esac
stop_machine

phase "the root destroyed, and rewritten from recovery"
# Armed the way `grub-editenv` arms it: one key set and every other line kept.
# Writing the block from nothing would drop the saved default with it, and the
# machine would come back on the fallback for a reason that had nothing to do
# with the repair.
arm_next() {
    ENV_AT="$("$REPO/build/tools/inspect/binfind" --offset \
        '# GRUB Environment Block' "$WORK")" || \
        fail "the disk has no environment block"
    {
        printf '# GRUB Environment Block\n'
        printf '# WARNING: Do not edit this file by tools other than grub-editenv!!!\n'
        # Every variable the block already holds except next_entry, which this
        # is about to set: the '#' fill is stripped from each line, the two
        # header comments and any empty line dropped.
        dd if="$WORK" bs=1 skip="$ENV_AT" count=1024 status=none |
            sed 's/#*$//' |
            grep -a -v -e '^#' -e '^$' -e '^next_entry='
        printf 'next_entry=%s\n' "$1"
    } > "$WORK.env"
    n="$(wc -c < "$WORK.env" | tr -d ' ')"
    dd if=/dev/zero bs=1 count=$((1024 - n)) 2>/dev/null | tr '\0' '#' \
        >> "$WORK.env"
    dd if="$WORK.env" of="$WORK" bs=1 seek="$ENV_AT" conv=notrunc status=none
    rm -f "$WORK.env"
}
# The root's superblock, destroyed with the machine stopped: an ext2 volume's
# identity is the third and fourth sectors of its partition. This is the damage
# no kernel entry can survive, since both boot this root.
dd if=/dev/zero of="$WORK" bs=512 seek=$((ROOT_OFF / 512 + 2)) count=2 \
    conv=notrunc status=none
# The damage is proved before it is repaired: the fallback entry, armed for one
# boot, must fail to mount and offer nothing, which is the state an operator
# would find the machine in. Booting straight into recovery would prove the
# repair without ever showing that anything was broken.
arm_next fallback
cold_watch broken "root: MBRID=1acef00d,3: /dev/disk0p3 has no ext2 file system this kernel mounts"
if grep -aq "rach2 login:" "$LOG"; then
    fail "a machine whose root is destroyed still reached a login"
fi
cleanup_boot
arm_next recovery

cold_boot recovery
grep -aq "modules: found" "$LOG" || fail "the armed entry did not load the recovery image"
OUT="$(machine 'test -b /dev/disk0 && echo RECOVERY""HERE' || true)"
case "$OUT" in
    *RECOVERYHERE*) ;;
    *) fail "the recovery entry did not come up with the whole-disk device: $OUT" ;;
esac
OUT="$(gzip -c "$ROOTIMG" | machine "gunzip | reflash /dev/disk0 $ROOT_OFF $ROOT_BYTES" || true)"
case "$OUT" in
    *"wrote $ROOT_BYTES bytes"*) ;;
    *) fail "the repair did not report the whole root written: $OUT" ;;
esac
# What landed is read from the disk file on the host, not from inside the
# machine: the bytes are the same bytes, and hashing 500 MiB on an emulated
# 32-bit processor took longer than any answer is worth waiting for. The
# machine's own read path is proved by the suites that mount this root.
machine 'sync' > /dev/null 2>&1 || true
stop_machine
OUT="$(dd if="$WORK" bs=512 skip="$ROOT_LBA" count="$ROOT_SECTORS" status=none | shasum -a 256 | awk '{print $1}')"
case "$OUT" in
    "$ROOT_SHA") ;;
    *) fail "the root written does not hash as the image sent: $OUT" ;;
esac

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
if grep -aq "modules: found" "$LOG"; then
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
echo "sealed-test: the sealed artifact came up on AHCI on its ext2 root and answered a key login; a kernel sent down that connection was installed, tried and committed; a destroyed candidate was caught by the fallback on the previous kernel and repaired from it; the root made missing, faulty and dirty under an armed trial each landed in recovery, whose init repaired it and the candidate was committed again; a staged menu reached the machine and a torn one left it booting; the boot loader was replaced with a marked core and then with another in the other tail, and the machine booted each; the root was destroyed and rewritten from recovery through one pipe, hashing identically on the platter; and a control copy did all of the ordinary work with no fallback and no recovery anywhere: OK"
echo "sealed-test: PASS"
