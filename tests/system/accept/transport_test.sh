#!/bin/sh
# Run the complete acceptance script against a private SSH fixture.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-accept-test.XXXXXX")
copy="$work/tests/system/accept"
mkdir -p "$copy" "$work/bin" "$work/build/images" "$work/build/krffs" "$work/krffs"
cp "$repo/tests/system/accept/metal_accept.sh" "$copy/original.sh"
cp "$copy/original.sh" "$copy/metal_accept.sh"
touch "$work/key"

# shellcheck disable=SC2329 # The exit trap restores the private mutation.
cleanup()
{
    cp "$copy/original.sh" "$copy/metal_accept.sh"
}
trap cleanup EXIT
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
    'cat > '*) cat >/dev/null; [ "$ACCEPT_CASE" != put-fail ] ;;
    'wc -c < '*) echo 1048576 ;;
    'loopctl attach '*) echo 1 ;;
    'mkdir -p /mnt/loop '*) printf 'hello from inside an image' ;;
    'echo written-on-the-machine '*) echo written-on-the-machine ;;
    'umount /mnt/loop '*) echo 0 ;;
    'cat /tmp/accept.fixture/loop.img') echo image ;;
    'cat /tmp/accept-transfer.fixture/data') echo image ;;
    'mv /tmp/accept-'*|'mv /tmp/accept.'*|'if [ -f /tmp/accept-transfer.'*) : ;;
    'rm -rf '*|'df -k /tmp;'*) : ;;
    'cat /proc/usb'*) : ;;
    *) echo "accept-fixture: unexpected command: $command" >&2; exit 99 ;;
esac
SSH
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
chmod +x "$work/bin/ssh" "$work/build/krffs/"*.krffs

PATH="$work/bin:$PATH"
ACCEPT_FIXTURE=$work
RACH2_ACCEPT_KEY="$work/key"
RACH2_ACCEPT_PASSWORD=unused-fixture-value
export PATH ACCEPT_FIXTURE RACH2_ACCEPT_KEY RACH2_ACCEPT_PASSWORD
unset RACH2_ACCEPT_DD RACH2_ACCEPT_BOOTLOG

check()
{
    ACCEPT_CASE=$1
    expected=$2
    export ACCEPT_CASE
    status=0
    log="$work/$phase.$RACH2_ACCEPT_SU.$ACCEPT_CASE.log"
    sh "$copy/metal_accept.sh" fixture > "$log" 2>&1 || status=$?
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
}

cases()
{
    for RACH2_ACCEPT_SU in 0 1; do
        export RACH2_ACCEPT_SU
        check good 0 || return 1
        for fault in cmdline-fail uname-fail hash-fail image-fail tmp-fail tmp-empty put-fail live-fault; do
            check "$fault" 1 || return 1
        done
        check live-clean 0 || return 1
        RACH2_ACCEPT_DD=/tmp/reader
        export RACH2_ACCEPT_DD
        check reader-override 2 || return 1
        unset RACH2_ACCEPT_DD
        RACH2_ACCEPT_BOOTLOG="$work/healthy-bootlog"
        export RACH2_ACCEPT_BOOTLOG
        check saved-bootlog 2 || return 1
        unset RACH2_ACCEPT_BOOTLOG
    done
    echo "accept-transport: $phase: 24 checks passed"
}

printf '%s\n' 'boot: Rach2 Booting' 'ext2: check: clean' > "$work/healthy-bootlog"
phase=fixed
cases
case "${1:-}" in
    '') ;;
    --mutate)
        # shellcheck disable=SC2016 # Match the variable in the private source.
        sed 's@: > "\$LOCAL_TMP/command-failed"@:@' "$copy/original.sh" > "$copy/metal_accept.sh"
        cmp -s "$copy/original.sh" "$copy/metal_accept.sh" && exit 1
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
        ;;
    *) echo 'accept-transport: usage: [--mutate]' >&2; exit 2 ;;
esac
echo "accept-transport: logs: $work"
