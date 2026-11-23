#!/bin/sh
# Profile the kernel time of single barecall kinds on Rach2 in QEMU.
#
# Usage: profile.sh OUT RACH2_KERNEL KIND:COUNT...
# Boots RACH2_KERNEL on the x86-64 os-real RAM image with the ext2 volume
# run.sh makes, mounted at /mnt, and for each KIND runs barecall on /mnt/f
# with that kind alone, COUNT times, while debugfs's bkl/profile samples
# the whole kernel; the console, with each kind's profile and big lock
# calls after a line `== profile KIND` and `== calls KIND`, goes to
# OUT/profile-KERNEL.log, KERNEL the first 8 digits of its SHA-256.
# bklread.sh names the samples by the kernel's kernel.debug.
# BARECALL_SOURCE=replay.c profiles the replay's kinds in the same way.
set -eu

[ $# -ge 3 ] || { echo 'usage: profile.sh OUT RACH2_KERNEL KIND:COUNT...' >&2; exit 2; }
tree=$(git rev-parse --show-toplevel)
step=$tree/dev-log/78-intel/78-11d/bare-call
out=$1 kernel=$2
shift 2
image=$tree/build/x86_64/images/os-real.krffs
profile=sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal
profile=$profile,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,extra_isize
test -f "$kernel"
test -f "$image"
mkdir -p "$out"

gcc -O2 -ffreestanding -static -nostdlib -fno-stack-protector -fno-pie -no-pie \
    -o "$out/barecall" "$step/${BARECALL_SOURCE:-barecall.c}"
rm -rf "$out/volume"
mkdir "$out/volume"
head -c 65536 /dev/urandom > "$out/volume/f"
mke2fs -q -F -t ext2 -b 4096 -I 256 -O "$profile" -d "$out/volume" "$out/volume.ext2" 64M
rm -r "$out/volume"

runs=
for spec in "$@"; do
    runs="$runs k=${spec%%:*}; n=${spec#*:}; for c in calls long tick; do echo 0 > \$B/\$c; done; echo kernel > \$B/profile; /tmp/barecall /mnt/f \$n 1 \$k; echo \"== profile \$k\"; cat \$B/profile; echo \"== calls \$k\"; cat \$B/calls;"
done
quiet='i=0; a=$(head -1 /proc/stat | cut -d" " -f5); while sleep 2; b=$(head -1 /proc/stat | cut -d" " -f5); [ $((b - a)) -gt 20 ] && [ $i -lt 60 ]; do a=$b; i=$((i + 1)); done'
name=$(shasum -a 256 "$kernel" | cut -c1-8)
BARECALL_QEMU="-drive file=$out/volume.ext2,format=raw,if=virtio,snapshot=on" \
    BARECALL_DONE='barecall: profiled' \
    BARECALL_CMD="mkdir -p /mnt && mount -t ext2 -o noatime /dev/disk0 /mnt && $quiet; B=/sys/kernel/debug/bkl; mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; $runs echo barecall:' 'profiled" \
    python3 -I "$step/serial.py" "$kernel" "$image" "$out/barecall" \
    "$tree/krffs/rach2_demo_password" "$out/profile-$name.log"
