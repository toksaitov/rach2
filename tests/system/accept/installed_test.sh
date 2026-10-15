#!/bin/sh
# installed_test.sh [KERNEL [HARNESS [WORK [ROW]]]]: boot a disk with a
# small installed ext2 root and run on it the rows of metal_accept.sh that
# read the command line, the disk and the ext2 root.
#
# The rows are copied out of metal_accept.sh between marker lines, most of
# them its section comments, so that the script and the test cannot
# differ. The disk is built by tools/image/mkdisk_grub.sh, with an MBR, and
# with a GUID table for the `gpt` row. The `candidate` and `gpt` rows must
# pass, and three rows must fail: a root on the wrong partition, a root
# mounted read-only, and a boot from the fallback slot. ROW runs one row;
# the default is all five.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
# The build tree of the architecture tests/system/suites.mk names, whose
# BusyBox and bootctl the root is made of.
ARCH_BUILD=${ARCH_BUILD:-build}
kernel=${1:-$ARCH_BUILD/rach2/kernel}
harness=${2:-build/tests/system/harness}
work=${3:-$ARCH_BUILD/images/metal-installed}
mkdir -p "$ARCH_BUILD/images" "$work"
work=$(CDPATH='' cd -- "$work" && pwd)
echo "metal-installed: logs: $work"
tree="$work/tree"
rm -rf "$tree"
mkdir -p "$tree/bin" "$tree/root" "$tree/tmp"
chmod 1777 "$tree/tmp"
cp "$ARCH_BUILD/vendor/busybox/busybox" "$tree/bin/busybox"
cp "$ARCH_BUILD/vendor/busybox/busybox" "$tree/bin/sh"
cp "$ARCH_BUILD/utils/sbin/bootctl" "$tree/bin/bootctl"

cat > "$tree/rows.sh" <<'ROWS'
#!/bin/sh
set -eu
PATH=/bin
export PATH
fails=0
note() { printf '  %s\n' "$1"; }
bad() { fails=$((fails + 1)); printf '  FAIL: %s\n' "$1"; }
run() { /bin/sh -c "$1"; }
BASELINE_MODEL='Samsung SSD 870 EVO 1TB'
BASELINE_SECTORS=1953525168
BASELINE_MBR_SHA256=652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3
BOOTLOG_OUT=/dev/bootlog
ROWS

awk '
    /^CMDLINE=\$\(run / { section = 1; starts++ }
    /^# ---- what it says it is/ { section = 0; ends++ }
    /^# ---- the disk, read/ { section = 1; starts++ }
    /^# Each invocation reads the running machine/ { section = 0; ends++ }
    /^# ---- the ext2 root, once/ { section = 1; starts++ }
    /^note "scratch: / { section = 0; ends++ }
    section { print }
    END { if (starts != 3 || ends != 3 || section) exit 1 }
' tests/system/accept/metal_accept.sh >> "$tree/rows.sh"
# shellcheck disable=SC2016 # The guest expands its failure count.
printf '\n[ "$fails" -eq 0 ]\n' >> "$tree/rows.sh"
sh -n "$tree/rows.sh"
cat > "$tree/check.sh" <<'CHECK'
#!/bin/sh
PATH=/bin
export PATH
printf 'cmdline: '; cat /proc/cmdline
cat /proc/mounts
/bin/sh /rows.sh
printf 'accept-status: %s\n' "$?"
echo METAL_INSTALLED_DONE
CHECK
chmod 755 "$tree/bin/"* "$tree/"*.sh

volume="$work/root.ext2"
rm -f "$volume"
profile='sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize'
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O "$profile" \
    -d "$tree" "$volume" 8M > "$work/mke2fs.log" 2>&1
e2fsck -fn "$volume" > "$work/fsck.log" 2>&1
rm -f "$work/bootctl.staged" "$work/rows.staged"
debugfs -R "dump /bin/bootctl $work/bootctl.staged" "$volume" > "$work/staged.log" 2>&1
cmp "$ARCH_BUILD/utils/sbin/bootctl" "$work/bootctl.staged"
debugfs -R "dump /rows.sh $work/rows.staged" "$volume" >> "$work/staged.log" 2>&1
cmp "$tree/rows.sh" "$work/rows.staged"
cksum "$kernel" "$work/bootctl.staged" "$work/rows.staged" > "$work/artifacts"

RACH2_DISK_TABLE=mbr RACH2_DISK_APPEND='rach2.rescue' RACH2_DISK_DHCP_APPEND='' \
    RACH2_DISK_BOOT_MB=16 RACH2_DISK_CLUSTER=1 RACH2_DISK_RECOVERY_MB=8 \
    RACH2_DISK_EXT2_MB=8 RACH2_DISK_EXT2_IMAGE="$volume" \
    sh tools/image/mkdisk_grub.sh "$kernel" "$work/base.disk" > "$work/disk.log" 2>&1
# The same root on a GUID table, which the boot line names by the table's disk
# GUID, as the installer does on such a disk.
RACH2_DISK_TABLE=gpt RACH2_DISK_APPEND='rach2.rescue' RACH2_DISK_DHCP_APPEND='' \
    RACH2_DISK_RECOVERY_MB=8 RACH2_DISK_EXT2_MB=8 RACH2_DISK_EXT2_IMAGE="$volume" \
    sh tools/image/mkdisk_grub.sh "$kernel" "$work/gpt.base.disk" > "$work/gpt.disk.log" 2>&1

# Report $1 and exit 1.
fail()
{
    echo "metal-installed: $1" >&2
    exit 1
}

# run_row ROW STATUS: prepare the disk for ROW, boot it, and require the
# acceptance status STATUS and the lines ROW must print.
run_row()
{
    row=$1
    expected=$2
    case "$row" in
        gpt) cp "$work/gpt.base.disk" "$work/$row.disk" ;;
        *) cp "$work/base.disk" "$work/$row.disk" ;;
    esac
    case "$row" in
        wrong-partition)
            python3 - "$work/$row.disk" <<'PY'
import sys

with open(sys.argv[1], 'r+b') as disk:
    disk.seek(478)
    entry = disk.read(16)
    disk.seek(478)
    disk.write(bytes(16) + entry)
PY
            mcopy -n -o -i "$work/$row.disk@@1048576" ::/boot/grub/grub.cfg "$work/grub.cfg"
            sed 's/rach2.root=MBRID=1acef00d,3/rach2.root=MBRID=1acef00d,4/g' \
                "$work/grub.cfg" > "$work/wrong.cfg"
            mcopy -o -i "$work/$row.disk@@1048576" "$work/wrong.cfg" ::/boot/grub/grub.cfg
            ;;
        readonly)
            cp "$volume" "$work/readonly.ext2"
            debugfs -w -R 'feature dir_index' "$work/readonly.ext2" > "$work/readonly.debugfs" 2>&1
            dd if="$work/readonly.ext2" of="$work/$row.disk" bs=1048576 seek=26 \
                conv=notrunc 2>/dev/null
            ;;
        fallback)
            mcopy -n -o -i "$work/$row.disk@@1048576" ::/boot/grub/grubenv "$work/grubenv"
            python3 - "$work/grubenv" "$work/fallback.env" <<'PY'
from pathlib import Path
import sys

env = Path(sys.argv[1]).read_bytes()
assert len(env) == 1024 and env.count(b'saved_entry=candidate\n') == 1
Path(sys.argv[2]).write_bytes(env.replace(b'saved_entry=candidate\n', b'saved_entry=fallback\n') + b'#')
PY
            mcopy -o -i "$work/$row.disk@@1048576" "$work/fallback.env" ::/boot/grub/grubenv
            ;;
    esac
    status=0
    "$harness" --bios --qemu-arg=-cpu --qemu-arg=max \
        --net hubport,id=net0,hubid=0 \
        --drive "file=$work/$row.disk,format=raw,if=ide,index=0" \
        --timeout 45 --steptmo 30 --log "$work/$row.serial.log" \
        --ok METAL_INSTALLED_DONE 'expect=Type .help.' \
        'send=exec /bin/sh /check.sh' 'expect=METAL_INSTALLED_DONE' \
        > "$work/$row.harness.log" 2>&1 || status=$?
    cat "$work/$row.harness.log"
    if [ "$status" -ne 0 ]; then
        cat "$work/$row.serial.log" >&2
    fi
    [ "$status" -eq 0 ] || fail "$row: guest did not finish"
    tr -d '\r' < "$work/$row.serial.log" > "$work/$row.log"
    sed -n '/^cmdline:/p; /^\/dev\/disk/p; /^  [^ ]/p; /^accept-status:/p' "$work/$row.log"
    grep -qx "accept-status: $expected" "$work/$row.log" || fail "$row: wrong acceptance status"
    grep -q '^  disk: the MBR hashes to [0-9a-f]\{64\}$' "$work/$row.log" ||
        fail "$row: the whole-disk window every boot publishes gave no MBR"
    if grep -q 'rach2.diskdiag' "$work/$row.log"; then
        fail "$row: unexpected diagnostic"
    fi
    case "$row" in
        candidate)
            grep -q '^  the ext2 root is mounted writable from /dev/disk0p3$' "$work/$row.log"
            grep -q '^  the ext2 root boot came from the candidate slot$' "$work/$row.log"
            grep -q '^  the ext2 root takes a file and gives it back$' "$work/$row.log"
            ;;
        gpt)
            grep -q '^cmdline: .*rach2.root=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,3 ' "$work/$row.log"
            grep -q '^  the ext2 root is mounted writable from /dev/disk0p3$' "$work/$row.log"
            grep -q '^  the ext2 root boot came from the candidate slot$' "$work/$row.log"
            grep -q '^  the ext2 root takes a file and gives it back$' "$work/$row.log"
            ;;
        wrong-partition)
            grep -q '^/dev/disk0p4 / ext2 rw,relatime' "$work/$row.log"
            grep -q '^  FAIL: the ext2 root is not the mounted root:' "$work/$row.log" \
                || fail "$row: missing root rejection"
            ;;
        readonly)
            grep -q '^/dev/disk0p3 / ext2 ro,relatime' "$work/$row.log"
            grep -q '^  FAIL: the ext2 root is not the mounted root:' "$work/$row.log" \
                || fail "$row: missing root rejection"
            ;;
        fallback)
            grep -q '^cmdline: .*rach2.slot=fallback' "$work/$row.log"
            grep -q '^  FAIL: the ext2 root boot did not come from the candidate slot:' "$work/$row.log"
            ;;
    esac
    echo "metal-installed: result: $row passed"
}

case "${4:-all}" in
    all)
        run_row candidate 0
        run_row gpt 0
        run_row wrong-partition 1
        run_row readonly 1
        run_row fallback 1
        echo 'metal-installed: result: all five installed rows passed'
        ;;
    candidate|gpt) run_row "$4" 0 ;;
    wrong-partition|readonly|fallback) run_row "$4" 1 ;;
    *) fail "unknown row: $4" ;;
esac
