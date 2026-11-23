#!/bin/sh
# Time bare system calls under a Linux kernel and Rach2 in the same QEMU.
#
# Usage: run.sh OUT LINUX_KERNEL LINUX_VIRTIO_BLK PAIRS RACH2_KERNEL...
# Each pair boots LINUX_KERNEL with barecall as its initramfs's /init and
# LINUX_VIRTIO_BLK, the kernel's virtio_blk.ko.xz, beside it, then each
# RACH2_KERNEL on the x86-64 os-real RAM image, where serial.py runs
# barecall on the console; the order of the Rach2 kernels turns each pair.
# Every boot has the same ext2 volume, in the profile of tools/image/
# mkext2.sh, on a virtio disk, mounted at /mnt without access times, and
# times /mnt/f; every boot uses multi-threaded TCG, -cpu max, four
# processors and 3 GiB. Each boot's console is OUT/linux-N.log or
# OUT/rach2-KERNEL-N.log, KERNEL the first 8 digits of its SHA-256, and the
# medians are printed at the end.
set -eu

[ $# -ge 5 ] || {
    echo 'usage: run.sh OUT LINUX_KERNEL LINUX_VIRTIO_BLK PAIRS RACH2_KERNEL...' >&2
    exit 2
}
tree=$(git rev-parse --show-toplevel)
step=$tree/dev-log/78-intel/78-11d/bare-call
out=$1 linux=$2 module=$3 pairs=$4
shift 4
image=$tree/build/x86_64/images/os-real.krffs
profile=sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal
profile=$profile,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,extra_isize
test -f "$linux"
test -f "$module"
test -f "$image"
mkdir -p "$out"

gcc -O2 -ffreestanding -static -nostdlib -fno-stack-protector -fno-pie -no-pie \
    -o "$out/barecall" "$step/barecall.c"
python3 -I "$step/initrd.py" "$out/barecall" "$out/initrd.gz" "$module"
rm -rf "$out/volume"
mkdir "$out/volume"
head -c 65536 /dev/urandom > "$out/volume/f"
mke2fs -q -F -t ext2 -b 4096 -I 256 -O "$profile" -d "$out/volume" "$out/volume.ext2" 64M
rm -r "$out/volume"
disk="-drive file=$out/volume.ext2,format=raw,if=virtio,snapshot=on"

# Boot Linux, whose /init times the calls and powers off, logging to $1.
boot_linux()
{
    timeout 900 qemu-system-x86_64 -accel tcg,thread=multi -cpu max -smp 4 \
        -m 3G -machine q35 -kernel "$linux" -initrd "$out/initrd.gz" \
        -append 'console=ttyS0 loglevel=7' -display none -vga none \
        -monitor none -no-reboot -nic none $disk -serial "file:$1"
    grep -q 'barecall: openclose' "$1"
}

# The guest's wait for the boot's own kernel work to end: until the system
# ticks of /proc/stat grow by 20 or fewer in 2 s, for at most 2 minutes.
quiet='i=0; a=$(head -1 /proc/stat | cut -d" " -f5); while sleep 2; b=$(head -1 /proc/stat | cut -d" " -f5); [ $((b - a)) -gt 20 ] && [ $i -lt 60 ]; do a=$b; i=$((i + 1)); done; echo "barecall: quiet after $((i * 2 + 2)) s"'

# Boot Rach2 kernel $1 and time the calls, logging to $2. A boot whose
# console stops answering is tried again, three times in all, and its log
# kept beside the next.
boot_rach2()
{
    tries=0
    until boot_rach2_once "$@"; do
        tries=$((tries + 1))
        [ "$tries" -lt 3 ] || return 1
        mv "$2" "$2.stalled-$tries"
    done
}

boot_rach2_once()
{
    BARECALL_QEMU=$disk \
        BARECALL_CMD="mkdir -p /mnt && mount -t ext2 -o noatime /dev/disk0 /mnt && $quiet && /tmp/barecall /mnt/f" \
        python3 -I "$step/serial.py" "$1" "$image" "$out/barecall" \
        "$tree/krffs/rach2_demo_password" "$2"
    grep -q 'barecall: openclose' "$2"
}

kernels=$*
i=1
while [ "$i" -le "$pairs" ]; do
    boot_linux "$out/linux-$i.log"
    printf 'barecall: linux %d done\n' "$i"
    order=$kernels
    if [ $((i % 2)) -eq 0 ]; then
        order=
        for k in $kernels; do order="$k $order"; done
    fi
    for k in $order; do
        name=$(shasum -a 256 "$k" | cut -c1-8)
        boot_rach2 "$k" "$out/rach2-$name-$i.log"
        printf 'barecall: rach2 %s %d done\n' "$name" "$i"
    done
    i=$((i + 1))
done
grep -H 'barecall: [a-z0-9]*: median' "$out"/*.log | tr -d '\r'
