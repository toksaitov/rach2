#!/bin/sh
# run.sh: 84-163's QEMU proof on the testing host, from the copy that holds
# 84-58's root: the root and the bundle on an ext2 partition, qemu.py's boot.
set -eu
# QEMU proof of 84-163's trial head (84-157's) through the Lenovo's root path: 84-58's
# trial root (d4d1da30...) as an image on an ext2 partition, booted through
# rach2.rootimage= on a loop device, with this head's bundle and Mac kernel.
W=build/tmp/t4889
rm -rf $W/d $W/part.img $W/disk.img; mkdir -p $W/d/trial4889/probes
IS=$(sha256sum build/x86_64/images/gnu-test.ext2 | cut -c1-16)
test "$IS" = d4d1da30b4f6cb50
cp --sparse=always build/x86_64/images/gnu-test.ext2 $W/d/trial4889/root-$IS.img
tar -xf $W/bundle.tar -C $W/d/trial4889/probes
grep -c -F 'echo "thread' $W/d/trial4889/probes/trip.sh; ls $W/d/trial4889/probes/run4889.sh $W/d/trial4889/probes/sample.py
mke2fs -q -t ext2 -L trialdata -d $W/d $W/part.img 28G
rm -rf $W/d
dd if=/dev/zero of=$W/disk.img bs=1M count=0 seek=$((28 * 1024 + 2)) status=none
dd if=$W/part.img of=$W/disk.img bs=1M seek=1 conv=notrunc,sparse status=none
rm -f $W/part.img
build/tools/image/mkmbr --lba $W/disk.img 2048:$((28 * 1024 * 2048)):83
sha256sum $W/kernel-mac $W/bundle.tar
launcher=$(sed -n 's/^launcher //p' /etc/rach2/etude-site)
export RACH2_WORKERS=1 RACH2_CPUS=4 RACH2_MEMORY_MIB=8192
s=0; "$launcher" legacy python3 $W/qemu.py p1 $W/kernel-mac root-$IS.img || s=$?
rm -f $W/disk.img
exit $s
