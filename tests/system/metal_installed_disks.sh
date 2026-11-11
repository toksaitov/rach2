#!/bin/sh
# metal_installed_disks.sh ARCH_BUILD DIR: make the disks of the metal cases
# that boot an installed ext2 root, tests/cases/machine.metal.json, in DIR,
# from the kernel, BusyBox and bootctl of the build tree ARCH_BUILD and the
# tree's metal_accept.sh.
#
# Each disk is made by tools/image/mkdisk_grub.sh with a small ext2 root
# that holds BusyBox as /bin/sh, bootctl, /rows.sh and /check.sh. /rows.sh
# is the rows of metal_accept.sh that read the command line, the disk and
# the ext2 root, copied out between marker lines, most of them its section
# comments, so that the script and the cases cannot differ; /check.sh,
# which the case runs at the debug shell, prints the command line, the
# mount table and what the rows print, then `accept-status: N` and
# `METAL_INSTALLED_DONE`.
#
# candidate.disk has an MBR, and gpt.disk the same root on a GUID table,
# which the boot line names by the table's disk GUID, as the installer does.
# Three more are the candidate's disk changed so that the rows must fail:
# wrong-partition.disk boots a root on the fourth partition,
# readonly.disk's root has a feature the driver mounts read-only, and
# fallback.disk boots the fallback slot.
set -eu
[ "$#" -eq 2 ] || { echo 'usage: metal_installed_disks.sh ARCH_BUILD DIR' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
build=$1
work=$2
kernel=$build/rach2/kernel
accept=tests/system/accept/metal_accept.sh
bootctl=$build/utils/sbin/bootctl
mkdir -p "$work"
work=$(CDPATH='' cd -- "$work" && pwd)
tree="$work/tree"
rm -rf "$tree"
mkdir -p "$tree/bin" "$tree/root" "$tree/tmp"
chmod 1777 "$tree/tmp"
cp "$build/vendor/busybox/busybox" "$tree/bin/busybox"
cp "$build/vendor/busybox/busybox" "$tree/bin/sh"
cp "$bootctl" "$tree/bin/bootctl"

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
' "$accept" >> "$tree/rows.sh"
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
cmp "$bootctl" "$work/bootctl.staged"
debugfs -R "dump /rows.sh $work/rows.staged" "$volume" >> "$work/staged.log" 2>&1
cmp "$tree/rows.sh" "$work/rows.staged"

RACH2_DISK_TABLE=mbr RACH2_DISK_APPEND='rach2.rescue' RACH2_DISK_DHCP_APPEND='' \
    RACH2_DISK_BOOT_MB=40 RACH2_DISK_CLUSTER=1 RACH2_DISK_RECOVERY_MB=8 \
    RACH2_DISK_EXT2_MB=8 RACH2_DISK_EXT2_IMAGE="$volume" \
    sh tools/image/mkdisk_grub.sh "$kernel" "$work/candidate.disk.new" > "$work/disk.log" 2>&1
RACH2_DISK_TABLE=gpt RACH2_DISK_APPEND='rach2.rescue' RACH2_DISK_DHCP_APPEND='' \
    RACH2_DISK_RECOVERY_MB=8 RACH2_DISK_EXT2_MB=8 RACH2_DISK_EXT2_IMAGE="$volume" \
    sh tools/image/mkdisk_grub.sh "$kernel" "$work/gpt.disk.new" > "$work/gpt.disk.log" 2>&1

# The root's entry moved from the third slot of the table to the fourth, and
# the boot line changed to name the fourth.
cp "$work/candidate.disk.new" "$work/wrong-partition.disk.new"
python3 - "$work/wrong-partition.disk.new" <<'PY'
import sys

with open(sys.argv[1], 'r+b') as disk:
    disk.seek(478)
    entry = disk.read(16)
    disk.seek(478)
    disk.write(bytes(16) + entry)
PY
mcopy -n -o -i "$work/wrong-partition.disk.new@@1048576" ::/boot/grub/grub.cfg "$work/grub.cfg"
sed 's/rach2.root=MBRID=1acef00d,3/rach2.root=MBRID=1acef00d,4/g' "$work/grub.cfg" > "$work/wrong.cfg"
mcopy -o -i "$work/wrong-partition.disk.new@@1048576" "$work/wrong.cfg" ::/boot/grub/grub.cfg

# The root with dir_index, which the driver does not write.
cp "$work/candidate.disk.new" "$work/readonly.disk.new"
cp "$volume" "$work/readonly.ext2"
debugfs -w -R 'feature dir_index' "$work/readonly.ext2" > "$work/readonly.debugfs" 2>&1
dd if="$work/readonly.ext2" of="$work/readonly.disk.new" bs=1048576 seek=26 conv=notrunc 2>/dev/null

# GRUB's saved entry changed to the fallback slot.
cp "$work/candidate.disk.new" "$work/fallback.disk.new"
mcopy -n -o -i "$work/fallback.disk.new@@1048576" ::/boot/grub/grubenv "$work/grubenv"
python3 - "$work/grubenv" "$work/fallback.env" <<'PY'
from pathlib import Path
import sys

env = Path(sys.argv[1]).read_bytes()
assert len(env) == 1024 and env.count(b'saved_entry=candidate\n') == 1
Path(sys.argv[2]).write_bytes(env.replace(b'saved_entry=candidate\n', b'saved_entry=fallback\n') + b'#')
PY
mcopy -o -i "$work/fallback.disk.new@@1048576" "$work/fallback.env" ::/boot/grub/grubenv

for row in candidate gpt wrong-partition readonly fallback; do
    mv "$work/$row.disk.new" "$work/$row.disk"
done
