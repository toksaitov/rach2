What this machine has, and how to look at it.

Four partitions on the disk:

  /dev/disk0p1   the boot files and the kernel slots   FAT32
  /dev/disk0p2   the fallback root                     KRFFS
  /dev/disk0p3   the recovery system                   FAT32
  /dev/disk0p4   the root you are running              ext2

Each of them has a /hello.txt in it. The root's is the one you can read
without mounting anything:

  cat /hello.txt

The other three are mounted read-only, read, and unmounted:

  mkdir -p /mnt/x
  mount -t vfat -o ro /dev/disk0p1 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
  mount -t krffs -o ro /dev/disk0p2 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
  mount -t vfat -o ro /dev/disk0p3 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x

Four file systems in files, in /demo, for writing to:

  fat32.img    a FAT32 volume, mounted as type vfat
  exfat.img    an exFAT volume, mounted as type exfat
  ext2.img     an ext2 volume
  krffs.img    a KRFFS volume
  empty.krffs  a formatted KRFFS volume with nothing in it

A file is not a device until a loop device stands on it:

  mkdir -p /mnt/k
  loopctl attach 0 /demo/krffs.img
  mount -t krffs /dev/loop0 /mnt/k
  cat /mnt/k/hello.txt
  echo mine >> /mnt/k/hello.txt
  sync
  umount /mnt/k
  loopctl detach 0

The same three lines mount the others, with -t vfat, -t exfat or -t ext2.
`loopctl` with no arguments prints what is attached, which is what
/proc/loop says. There are four loop devices and two KRFFS volumes may be
mounted at once.

An image of your own is a copy of the empty one:

  cp /demo/empty.krffs /root/mine.krffs
  loopctl attach 0 /root/mine.krffs
  mount -t krffs /dev/loop0 /mnt/k
  echo 'hello, world' > /mnt/k/hello.txt
  mkdir /mnt/k/notes
  sync
  umount /mnt/k
  loopctl detach 0

There is no mkfs on this machine, so an image of another size is made on
the host with krffs/mkimage.sh and copied here.

What a file under a loop device will not take while it is attached: a
write, a truncation, a rename over it or a removal. Unmount, then detach,
then it is an ordinary file again.
