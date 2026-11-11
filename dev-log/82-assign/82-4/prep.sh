#!/bin/sh
# prep.sh DIR: on Debian on the Lenovo's p5, as root, put step 11's guest of
# the second discovery run on the data partition, p6, under DIR, for the
# QEMU of Rach2's trial root: the guest's disk as one qcow2 image with no
# backing file, its kernel, initrd and NoCloud seed, and in the image's copy
# g82.sh, g82.service and weston.ini from this script's directory, the unit
# enabled. Step 11's own files are only read.
#
# It prints each file's SHA-256 and "prep: result: pass", or stops at the
# first failure with "prep: result: fail: REASON". Run again, it starts the
# copy over.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dir=${1:?usage: prep.sh DIR}
s=/srv/run2/step11
seed=/srv/run2/step10/seed/seed.img
p6=/dev/sda6
mnt=/mnt/t2666-p6
img=/mnt/t2666-guest
nbd=/dev/nbd0

say() { echo "prep: $*"; }
fail() { say "result: fail: $*"; exit 1; }
cleanup() {
    mountpoint -q $img && umount $img
    [ -e /sys/block/nbd0/pid ] && qemu-nbd --disconnect $nbd > /dev/null
    mountpoint -q $mnt && umount $mnt
    :
}
trap cleanup EXIT

[ "$(id -u)" = 0 ] || fail 'not root'
[ "$(findmnt -n -o SOURCE /)" = /dev/sda5 ] || fail '/ is not /dev/sda5'
[ "$(blkid -o value -s LABEL $p6)" = 'Rach2 data' ] || fail "$p6 is not Rach2's data partition"
! findmnt -n $p6 > /dev/null || fail "$p6 is mounted"
for f in $s/guest.qcow2 $seed "$here/g82.sh" "$here/g82.service" "$here/weston.ini"; do
    [ -f "$f" ] || fail "no $f"
done
kernel=$(ls $s/boot/vmlinuz-* | head -n 1)
initrd=$(ls $s/boot/initrd.img-* | head -n 1)
[ -f "$kernel" ] && [ -f "$initrd" ] || fail "no kernel and initrd in $s/boot"
qemu-img info --backing-chain $s/guest.qcow2 | grep -E '^(image|virtual size|disk size|backing file):'

mkdir -p $mnt $img
mount -t ext2 $p6 $mnt
out=$mnt/${dir#/}
rm -rf "$out"
mkdir -p "$out"
say "copy: the guest's disk into $dir/guest.qcow2"
qemu-img convert -O qcow2 $s/guest.qcow2 "$out/guest.qcow2"
cp "$kernel" "$out/vmlinuz"
cp "$initrd" "$out/initrd.img"
cp $seed "$out/seed.img"

say 'install: g82 in the copy'
modprobe nbd max_part=8
qemu-nbd --connect=$nbd "$out/guest.qcow2"
udevadm settle
n=0
while [ ! -b ${nbd}p1 ] && [ $n -lt 20 ]; do sleep 0.5; n=$((n + 1)); done
mount ${nbd}p1 $img
grep -q '^gfx:' $img/etc/passwd || fail 'the guest has no gfx'
for f in /usr/bin/weston /usr/bin/glmark2-wayland /usr/bin/glmark2-es2-wayland /usr/bin/seatd; do
    [ -x $img$f ] || fail "the guest has no $f"
done
install -m 755 "$here/g82.sh" $img/usr/local/sbin/g82.sh
install -m 644 "$here/g82.service" $img/etc/systemd/system/g82.service
install -d $img/etc/g82
install -m 644 "$here/weston.ini" $img/etc/g82/weston.ini
ln -sf /etc/systemd/system/g82.service $img/etc/systemd/system/multi-user.target.wants/g82.service
say "guest: Debian $(cat $img/etc/debian_version)"
dpkg-query --admindir=$img/var/lib/dpkg -W glmark2-wayland glmark2-es2-wayland weston \
    libgl1-mesa-dri seatd
umount $img
qemu-nbd --disconnect $nbd > /dev/null
sleep 1

qemu-img check "$out/guest.qcow2"
(cd "$out" && sha256sum guest.qcow2 vmlinuz initrd.img seed.img && ls -l)
df -k $mnt
sync
umount $mnt
e2fsck -fn $p6
say 'result: pass'
