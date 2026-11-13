# Stage the GNU trial root and run its first graphics loop under KVM.
set -eu
# QEMU proof of 84-167's trial head through the Lenovo's root path: 84-58's
# trial root (d4d1da30...) as an image on an ext2 partition, booted through
# rach2.rootimage= on a loop device, with 84-163's bundle and the Mac kernel.
W=build/tmp/4893-proof
SRC=/home/testing/rach2/build/worktrees/t4733/build/x86_64/images/gnu-test.ext2
trap 'rm -rf "$W/d" "$W/part.img" "$W/disk.img"' EXIT
mkdir -p $W/d/trial4893/probes
echo "4893: hashing the existing trial root"
ROOT_SHA=$(sha256sum $SRC | cut -d" " -f1)
test "$ROOT_SHA" = d4d1da30b4f6cb509d2e9e8122aea4749cd80917438a012ed0228cdbeab95a8c
echo "4893: root $ROOT_SHA"
IS=$(echo "$ROOT_SHA" | cut -c1-16)
echo "4893: staging the trial partition"
cp --sparse=always $SRC $W/d/trial4893/root-$IS.img
tar -xf $W/bundle.tar -C $W/d/trial4893/probes
mke2fs -q -t ext2 -L trialdata -d $W/d $W/part.img 28G
rm -rf "$W/d"
dd if=/dev/zero of=$W/disk.img bs=1M count=0 seek=$((28 * 1024 + 2)) status=none
dd if=$W/part.img of=$W/disk.img bs=1M seek=1 conv=notrunc,sparse status=none
rm -f "$W/part.img"
build/tools/image/mkmbr --lba $W/disk.img 2048:$((28 * 1024 * 2048)):83
sha256sum $W/kernel-mac $W/bundle.tar
echo "4893: starting the trial kernel"
launcher=$(sed -n 's/^launcher //p' /etc/rach2/etude-site)
export RACH2_WORKERS=1 RACH2_CPUS=4 RACH2_MEMORY_MIB=8192
s=0; "$launcher" legacy python3 $W/qemu.py p1 $W/kernel-mac root-$IS.img || s=$?
exit $s
