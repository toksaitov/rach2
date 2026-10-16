#!/bin/sh
# transport_test.sh: run the whole of metal_accept.sh, in a tree with no
# build/images, against a stand-in for ssh, which answers each command the
# script sends as a machine would, in 17 cases, once with root's key and once
# through su_command.sh.
#
# A case fails a command, a transfer, a hash or a check of the stick, or
# boots an ext2 root whose check found a fault, and the script must then
# exit 1 and never print PASS; RACH2_ACCEPT_DD and RACH2_ACCEPT_BOOTLOG,
# which the script refuses, must exit 2. With --mutate, two faults in a
# private copy of the script must be caught: a failed command left
# unrecorded, and a stick matched by the prefix of its node, which writes
# disk10 when disk1 is named. --texts checks that the text each fault
# replaces is once in metal_accept.sh.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" accept-transport "$repo/tests/system/accept/transport_test.sh" "$@"
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept-test.XXXXXX")
copy="$work/tests/system/accept"
mkdir -p "$copy" "$work/bin" "$work/build/krffs" "$work/krffs"
cp "$repo/tests/system/accept/metal_accept.sh" "$copy/original.sh"
cp "$copy/original.sh" "$copy/metal_accept.sh"
touch "$work/key"

# shellcheck disable=SC2329 # The exit trap restores the private mutation.
cleanup()
{
    cp "$copy/original.sh" "$copy/metal_accept.sh"
}
trap cleanup EXIT

# transport_edit FAULT: the private metal_accept.sh is the saved one with FAULT
# in it: no failure record (record) or a disk matched by its prefix alone
# (disk-prefix); fail when the edit changes nothing, since its text has left
# the script.
transport_edit()
{
    case $1 in
        record)
            # shellcheck disable=SC2016 # The private source's variable.
            sed 's@: > "\$LOCAL_TMP/command-failed"@:@' "$copy/original.sh" > "$copy/metal_accept.sh" ;;
        disk-prefix)
            # shellcheck disable=SC2016 # Match the private source's variable.
            sed 's@grep -E "\^/dev/\$STICK(p\[0-9\]+)?:"@grep "^/dev/$STICK"@' \
                "$copy/original.sh" > "$copy/metal_accept.sh" ;;
    esac
    if cmp -s "$copy/original.sh" "$copy/metal_accept.sh"; then
        echo "accept-transport: the $1 fault is not in metal_accept.sh" >&2
        exit 1
    fi
}

if [ "${1:-}" = --texts ]; then
    transport_edit record
    transport_edit disk-prefix
    trap - EXIT
    rm -rf "$work"
    echo 'accept-transport: texts: every fault is once in its source'
    exit 0
fi
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

cat > "$work/bin/ssh" <<'SSH'
#!/bin/sh
set -eu
for command do :; done
printf '%s\n' "$command" >> "$ACCEPT_FIXTURE/calls"
case "$command" in
    'echo ALIVE') echo ALIVE ;;
    'id -u') echo 0 ;;
    'cat /proc/cmdline')
        [ "$ACCEPT_CASE" != cmdline-fail ] || exit 7
        echo 'rach2.ip=none'
        ;;
    'cat /proc/mounts')
        case "$ACCEPT_CASE" in
            live-fault|live-clean) echo '/dev/disk0p3 / ext2 rw,relatime' ;;
            *) echo 'none / krffs rw' ;;
        esac
        ;;
    'uname -a') echo Rach2; [ "$ACCEPT_CASE" != uname-fail ] ;;
    'id') echo 'uid=0(root)' ;;
    'cat /proc/uptime'*) echo '1.00 0.00' ;;
    'dd if=/dev/urandom'*) echo 16 ;;
    'cat /proc/disk'*)
        printf '%s\n' 'policy read-only' 'disk 0 usb 0 1000 mbr fixture' \
            'identity 0 512 removable free FIXTURE'
        if [ "$ACCEPT_CASE" = disk-prefix ] || [ "${ACCEPT_CASE#fat-}" != "$ACCEPT_CASE" ]; then
            printf '%s\n' 'disk 1 usb 0 1000 mbr approved' \
                'identity 1 512 removable free APPROVED01' \
                'disk 10 usb 0 1000 mbr unapproved' \
                'identity 10 512 removable free UNAPPROVED10'
        fi
        ;;
    'if [ -b /dev/disk0 ]'*) echo published ;;
    *'accept-mbr.'*)
        printf '%064d  /tmp/sector\n' 0
        [ "$ACCEPT_CASE" != hash-fail ]
        ;;
    'cat /dev/bootlog')
        printf '%s\n' 'boot: Rach2 Booting' 'usb: controller: none' 'lapic: ready: fixture'
        case "$ACCEPT_CASE" in
            live-fault)
                echo 'ext2: check: 1 lost blocks, 1 orphan inodes, kept for e2fsck'
                echo 'ext2: check: clean'
                ;;
            live-clean) echo 'ext2: check: clean' ;;
        esac
        ;;
    'cat /proc/interrupts'*) printf '32:\t10\t0\tpit\n' ;;
    'ls /dev'*) echo disk0 ;;
    'bootctl status 2>/dev/null || :') : ;;
    'bootctl status') echo 'booted: candidate' ;;
    *'mktemp /root/accept.'*) echo accept ;;
    'cat /proc/filesystems') echo krffs ;;
    'mktemp -d /tmp/accept-transfer.'*) echo /tmp/accept-transfer.fixture ;;
    'mktemp -d /tmp/accept.'*)
        case "$ACCEPT_CASE" in
            tmp-fail) exit 7 ;;
            tmp-empty) exit 0 ;;
            *) echo /tmp/accept.fixture ;;
        esac
        ;;
    'cat > '*) cat > "$ACCEPT_FIXTURE/payload"; [ "$ACCEPT_CASE" != put-fail ] ;;
    'wc -c < '*) echo 1048576 ;;
    'loopctl attach '*) echo 1 ;;
    'mkdir -p /mnt/loop '*) printf 'hello from inside an image' ;;
    'echo written-on-the-machine '*) echo written-on-the-machine ;;
    'umount /mnt/loop '*) echo 0 ;;
    'cat /tmp/accept.fixture/loop.img') echo image ;;
    'cat /tmp/accept-transfer.fixture/data') echo image ;;
    'mv /tmp/accept-'*|'mv /tmp/accept.'*|'if [ -f /tmp/accept-transfer.'*) : ;;
    'rm -rf '*|'df -k /tmp;'*) : ;;
    'cat /proc/usb'*)
        if [ "$ACCEPT_CASE" = disk-prefix ] || [ "${ACCEPT_CASE#fat-}" != "$ACCEPT_CASE" ]; then
            for disk in disk1 disk10; do
                flushes=0
                cmp -s "$ACCEPT_FIXTURE/$disk.img" "$ACCEPT_FIXTURE/$disk.before" || flushes=1
                echo "$disk device 1 flushes $flushes"
            done
        fi
        ;;
    'ls -l /dev/disk1 2>/dev/null') echo 'brw------- disk1' ;;
    'blkid 2>/dev/null')
        printf '%s\n' '/dev/disk10p1: LABEL="TESTVOL" TYPE="vfat"' \
            '/dev/disk1p1: LABEL="TESTVOL" TYPE="vfat"'
        ;;
    'mkdir /tmp/accept.fixture/mnt && mount '*|'mount /dev/disk'*' /tmp/accept.fixture/mnt')
        node=${command#*mount }
        node=${node%% *}
        case "$node" in
            /dev/disk1p1) disk=disk1 ;;
            /dev/disk10p1) disk=disk10 ;;
            *) exit 99 ;;
        esac
        echo "$disk" > "$ACCEPT_FIXTURE/selected"
        ;;
    'mktemp /tmp/accept.fixture/mnt/accept.'*) echo /tmp/accept.fixture/mnt/accept.fixture ;;
    'cp /tmp/accept.fixture/expected '*)
        disk=$(cat "$ACCEPT_FIXTURE/selected")
        printf 'accept\n' >> "$ACCEPT_FIXTURE/$disk.img"
        ;;
    'cmp /tmp/accept.fixture/expected '*) [ "$ACCEPT_CASE" != fat-remount ] ;;
    'umount /tmp/accept.fixture/mnt'*) [ "$ACCEPT_CASE" != fat-busy ] ;;
    'rm -f /tmp/accept.fixture/mnt/accept.'*) : ;;
    'rm /tmp/accept.fixture/mnt/accept.'*) : ;;
    'if [ -b /dev/disk1p'*) : ;;
    *) echo "accept-fixture: unexpected command: $command" >&2; exit 99 ;;
esac
SSH
cat > "$work/bin/mcopy" <<'MCOPY'
#!/bin/sh
set -eu
for destination do :; done
: > "$ACCEPT_FIXTURE/host-read"
cp "$ACCEPT_FIXTURE/payload" "$destination"
if [ "$ACCEPT_CASE" = fat-host ]; then
    printf x | dd of="$destination" bs=1 count=1 conv=notrunc 2>/dev/null
fi
MCOPY
cat > "$copy/su_command.sh" <<'SU'
#!/bin/sh
# Model only the transport status. The metal suite runs the real su helper.
exec ssh "$6"
SU
cat > "$work/krffs/mkimage.sh" <<'IMAGE'
#!/bin/sh
[ "$ACCEPT_CASE" != image-fail ] || exit 1
printf image > "$2"
IMAGE
printf '#!/bin/sh\nexit 0\n' > "$work/build/krffs/mkfs.krffs"
cp "$work/build/krffs/mkfs.krffs" "$work/build/krffs/fsck.krffs"
cat > "$work/build/krffs/readfile.krffs" <<'READFILE'
#!/bin/sh
echo written-on-the-machine > "$3"
READFILE
chmod +x "$work/bin/ssh" "$work/bin/mcopy" "$work/build/krffs/"*.krffs

PATH="$work/bin:$PATH"
ACCEPT_FIXTURE=$work
RACH2_ACCEPT_KEY="$work/key"
RACH2_ACCEPT_PASSWORD=unused-fixture-value
export PATH ACCEPT_FIXTURE RACH2_ACCEPT_KEY RACH2_ACCEPT_PASSWORD
unset RACH2_ACCEPT_DD RACH2_ACCEPT_BOOTLOG RACH2_ACCEPT_FAT_IMAGE

# check CASE STATUS: run the private script for CASE under $phase and
# $RACH2_ACCEPT_SU, and require STATUS (1 means any failure) and the lines
# the case must print.
check()
{
    ACCEPT_CASE=$1
    expected=$2
    export ACCEPT_CASE
    status=0
    log="$work/$phase.$RACH2_ACCEPT_SU.$ACCEPT_CASE.log"
    rm -f "$work/host-read"
    if [ "$ACCEPT_CASE" = disk-prefix ] || [ "${ACCEPT_CASE#fat-}" != "$ACCEPT_CASE" ]; then
        for disk in disk1 disk10; do
            printf '%s original bytes\n' "$disk" > "$work/$disk.img"
            cp "$work/$disk.img" "$work/$disk.before"
        done
        sh "$copy/metal_accept.sh" fixture 22 APPROVED01 > "$log" 2>&1 || status=$?
        if ! cmp -s "$work/disk10.img" "$work/disk10.before"; then
            echo "accept-transport: $phase/$RACH2_ACCEPT_SU/disk-prefix: disk10 was written" >&2
            return 1
        fi
        printf 'disk1 original bytes\naccept\n' > "$work/disk1.expected"
        cmp -s "$work/disk1.img" "$work/disk1.expected" || return 1
        if [ "$expected" -eq 0 ]; then
            grep -q 'stick: the volume TESTVOL on /dev/disk1p1 mounts' "$log" || return 1
        fi
    else
        sh "$copy/metal_accept.sh" fixture > "$log" 2>&1 || status=$?
    fi
    if { [ "$expected" -eq 1 ] && [ "$status" -eq 0 ]; } ||
        { [ "$expected" -ne 1 ] && [ "$status" -ne "$expected" ]; }; then
        cat "$log" >&2
        echo "accept-transport: $phase/$RACH2_ACCEPT_SU/$ACCEPT_CASE returned $status, expected $expected" >&2
        return 1
    fi
    if [ "$expected" -eq 0 ]; then
        grep -qx 'metal-accept: PASS' "$log" || return 1
    elif grep -q '^metal-accept: PASS' "$log"; then
        echo "accept-transport: $ACCEPT_CASE printed PASS after failure" >&2
        return 1
    fi
    if [ "$ACCEPT_CASE" = hash-fail ] && grep -q 'the MBR hashes to' "$log"; then
        echo 'accept-transport: a failed reader supplied a digest' >&2
        return 1
    fi
    case "$ACCEPT_CASE" in
        fat-busy|fat-remount)
            grep -q 'stick: cached bytes match' "$log" || return 1
            [ ! -f "$work/host-read" ] || return 1
            if grep -q 'stick: remount bytes match' "$log"; then return 1; fi
            ;;
        fat-good|fat-host)
            grep -q 'stick: remount bytes match after successful unmounts' "$log" || return 1
            [ -f "$work/host-read" ] || return 1
            ;;
    esac
    case "$ACCEPT_CASE" in
        fat-busy) grep -q 'FAIL: stick: first unmount failed' "$log" || return 1 ;;
        fat-remount) grep -q 'FAIL: stick: remount bytes differ' "$log" || return 1 ;;
        fat-host) grep -q 'FAIL: stick: host FAT bytes differ' "$log" || return 1 ;;
        fat-good) grep -q 'stick: host FAT bytes match while unmounted' "$log" || return 1 ;;
    esac
}

# Run every case with root's key and then through su.
cases()
{
    for RACH2_ACCEPT_SU in 0 1; do
        export RACH2_ACCEPT_SU
        check good 0 || return 1
        for fault in cmdline-fail uname-fail hash-fail image-fail tmp-fail tmp-empty put-fail live-fault; do
            check "$fault" 1 || return 1
        done
        check live-clean 0 || return 1
        check disk-prefix 0 || return 1
        RACH2_ACCEPT_FAT_IMAGE=fixture
        export RACH2_ACCEPT_FAT_IMAGE
        check fat-good 0 || return 1
        for fault in fat-busy fat-remount fat-host; do
            check "$fault" 1 || return 1
        done
        unset RACH2_ACCEPT_FAT_IMAGE
        RACH2_ACCEPT_DD=/tmp/reader
        export RACH2_ACCEPT_DD
        check reader-override 2 || return 1
        unset RACH2_ACCEPT_DD
        RACH2_ACCEPT_BOOTLOG="$work/healthy-bootlog"
        export RACH2_ACCEPT_BOOTLOG
        check saved-bootlog 2 || return 1
        unset RACH2_ACCEPT_BOOTLOG
    done
    echo "accept-transport: $phase: 34 checks passed"
}

printf '%s\n' 'boot: Rach2 Booting' 'ext2: check: clean' > "$work/healthy-bootlog"
phase=fixed
cases
case "${1:-}" in
    '') ;;
    --mutate)
        transport_edit record
        phase=mutated
        if cases > "$work/mutation.log" 2>&1; then
            echo 'accept-transport: the missing failure record was not detected' >&2
            exit 1
        fi
        grep -q 'uname-fail returned 0, expected 1' "$work/mutation.log"
        echo 'accept-transport: mutation: the missing failure record was detected'
        cp "$copy/original.sh" "$copy/metal_accept.sh"
        phase=restored
        cases
        transport_edit disk-prefix
        phase='disk-mutated'
        if cases > "$work/disk-mutation.log" 2>&1; then
            echo 'accept-transport: the disk prefix mutation was not detected' >&2
            exit 1
        fi
        grep -q 'disk-prefix: disk10 was written' "$work/disk-mutation.log"
        echo 'accept-transport: mutation: the disk prefix match wrote disk10 and was detected'
        cp "$copy/original.sh" "$copy/metal_accept.sh"
        phase='disk-restored'
        cases
        ;;
    *) echo 'accept-transport: usage: [--mutate | --texts]' >&2; exit 2 ;;
esac
echo "accept-transport: logs: $work"
