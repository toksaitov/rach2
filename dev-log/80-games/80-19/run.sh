#!/bin/sh
#
# run.sh: boot each kernel named on the command line, of the architecture
# ARCH names (x86_64 when unset, or i686), in QEMU with four processors,
# spliceload.c as init and an ext2 disk of its own, and keep the serial
# output as build/runs/task3489/ARCH-NAME.log, NAME the kernel's file name.
#
# spliceload warms a 256 MiB file and copies 16 MiB of it by read(2) and
# write(2) and by splice through a pipe, alone and beside three processes
# pinned to the other processors that pread the same cached mebibyte in a
# loop, each under the big lock; the copier is pinned to the boot
# processor. It powers the machine off when it is done, so each boot ends by
# itself. Run from the top of the tree after `make ARCH=...`, which builds
# musl and the KRFFS tools.
#
# Usage: [ARCH=i686] sh dev-log/80-games/80-19/run.sh KERNEL ...
#
set -eu

dir=build/tmp/task3489
runs=build/runs/task3489
mke2fs=$(command -v mke2fs || echo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs)
arch=${ARCH:-x86_64}
qemu=qemu-system-x86_64
[ "$arch" = i686 ] && qemu=qemu-system-i386

mkdir -p "$dir" "$runs"
TMPDIR="$PWD/$dir" RACH2_ARCH=$arch sh vendor/musl/muslgcc -O2 -Wall -Wextra -Werror \
    -Wl,-z,noexecstack -o "$dir/spliceload" dev-log/80-games/80-19/spliceload.c
cat > "$dir/root.manifest" <<EOF
file 0755 /sbin/init $dir/spliceload
text 0644 /spliceload.conf 256 16 3 2
EOF
dd if=/dev/zero of="$dir/root.krffs" bs=1048576 count=4 2> /dev/null
build/krffs/mkfs.krffs "$dir/root.krffs" -f
build/krffs/stageimage.krffs "$dir/root.manifest" "$dir/root.krffs"

for kernel in "$@"; do
    name=$arch-$(basename "$kernel")
    rm -f "$dir/data.ext2"
    : > "$dir/data.ext2"
    "$mke2fs" -q -t ext2 -b 4096 -O '^dir_index,^ext_attr' -F "$dir/data.ext2" 131072
    "$qemu" -accel tcg,thread=multi -cpu max -smp 4 -m 1536 -display none \
        -monitor none -serial stdio -nic none -kernel "$kernel" -initrd "$dir/root.krffs" \
        -drive "file=$dir/data.ext2,format=raw,if=ide" -append 'rach2.loglevel=3 rach2.tscreliable' \
        -no-reboot > "$runs/$name.log" 2>&1
    grep -E 'spliceload: (result|load)|SPLICELOAD' "$runs/$name.log" || true
done
rm -f "$dir/data.ext2"
