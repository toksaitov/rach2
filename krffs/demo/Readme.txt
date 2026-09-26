What this machine has, and how to look at it.

Three partitions on the disk:

  /dev/disk0p1   the boot files and the two kernel slots   FAT32
  /dev/disk0p2   the recovery system                       FAT32
  /dev/disk0p3   the root you are running                  ext2

The two FAT partitions each have a /hello.txt, and the root has
/demo/hello.txt, all reading the same line. Run what follows as root. The
running root's copy is read without mounting anything:

  cat /demo/hello.txt

The other two are mounted read-only, read, and unmounted:

  mkdir -p /mnt/x
  mount -t vfat -o ro /dev/disk0p1 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
  mount -t vfat -o ro /dev/disk0p2 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x

Four file systems in files, in /demo, for writing to:

  fat32.img    a FAT32 volume, mounted as type vfat
  exfat.img    an exFAT volume, mounted as type exfat
  ext2.img     an ext2 volume
  krffs.img    a KRFFS volume
  empty.krffs  a formatted KRFFS volume with nothing in it

An image file is mounted by attaching it to a loop device and mounting the
device:

  mkdir -p /mnt/k
  loopctl attach 0 /demo/krffs.img
  mount -t krffs /dev/loop0 /mnt/k
  cat /mnt/k/hello.txt
  echo 'written on the machine' >> /mnt/k/hello.txt
  sync
  umount /mnt/k
  loopctl detach 0

The others go the same way, each with its own file and type: fat32.img as
vfat, exfat.img as exfat, ext2.img as ext2. Changing only the -t leaves the
KRFFS image attached and the wrong driver reading it. `loopctl` with no
arguments says which devices are taken, which is what /proc/loop says; use
a free number in both commands. There are four loop devices, and only
memory limits how many volumes are mounted at once.

An image of your own is a copy of the empty one:

  cp /demo/empty.krffs /root/mine.krffs
  loopctl attach 0 /root/mine.krffs
  mount -t krffs /dev/loop0 /mnt/k
  echo 'hello, world' > /mnt/k/hello.txt
  mkdir /mnt/k/notes
  sync
  umount /mnt/k
  loopctl detach 0

There is no KRFFS formatter on this machine, so an image of another size
is made on the host with krffs/mkimage.sh and copied here.

A write or a truncation of an attached image answers EBUSY. On ext2 the
file can still be renamed or unlinked, and the image survives that because
the loop device holds its inode; unmount and detach before replacing or
removing it.
