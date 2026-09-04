# Milestone 44 on the machine

The Lenovo G50-70 at 192.168.0.106, its ext2 root on `/dev/disk0p4`, reached
over SSH from the mac mini. What follows is what the machine did, in the
order it did it.

## The kernel

The first install put `build/rach2-metal/kernel` in the candidate slot. That
build refuses every write to the disk (`disk: policy: read-only`), so the
ext2 root could not be mounted and the machine refused to boot:

```
ahci: refused: command 0x35: rach2: disk policy: read-only
ext2: root: /dev/disk0p4 refused
boot: refused: no file system mounted
```

The same line reproduces in QEMU against `build/images/rach2-sealed.disk`,
which is where it was found. The trial cost one boot: `bootctl kernel` had
set the saved default to the fallback, and the machine came back on the old
kernel with nothing written. The ordinary build, `build/rach2/kernel`, is the
one this machine takes; it boots the ext2 root and reaches `/sbin/init`.

The second install was that build. The trial booted, `/proc/filesystems`
listed `krffs`, `/dev` had `loop-control`, and `bootctl commit` made it the
saved default.

The third install carries the xHCI completion code into the mass-storage
failure message, for the stick's fault below. It went in the same way and was
committed after it came up on the ext2 root.

## A file system in a file, on the machine

An image built here by `krffs/mkimage.sh` (4 MiB, two files), copied to
`/root/loop.img` on the ext2 root:

```
loopctl attach 0 /root/loop.img     ->  loop0 4194304 rw /root/loop.img
mount -t krffs /dev/loop0 /mnt/loop ->  /dev/loop0 /mnt/loop krffs rw
cat /mnt/loop/hello.txt             ->  hello from inside an image
echo ... > /mnt/loop/accept.txt     ->  written back and read
umount, loopctl detach 0            ->  /proc/loop empty, no /dev/loop0
```

The image copied back to the host: `fsck.krffs --strict` accepts it, and
`readfile.krffs` reads `/accept.txt` and `/hello.txt` out of it with the
bytes the machine wrote. `tests/system/accept/metal_accept.sh` runs the same
sequence and passes end to end, with every other row of the acceptance
unchanged.

## The stick, and what it did under a sustained write

The stick is a SanDisk 3.2Gen1, 120164352 sectors, on port 10.1 of a
SuperSpeed hub, carrying a Rach2 ISO and a FAT32 volume labelled RACH2SAVE.
Small writes to that volume work: 64 KiB and 256 KiB files are written and
read back. A 1 MiB write does not:

```
usbdisk: recovery: device 10: the data stage failed
usbdisk: device 10: recovery never finished
usbdisk: gone: device 10
bcache: write: sector 35608 failed (-5), kept dirty
vfs: read-only: /mnt/stick: write of sector 35608 failed (-5)
usbdisk: found: device 11 on endpoints 3 and 4
usbdisk: device 11: a transfer never answered
usbdisk: refused: device 11: it stopped answering while it was being brought up
```

The data stage of a bulk transfer fails with neither a stall nor a short
answer, the mass-storage reset that follows does not finish inside its
deadline, and the device is dropped. It re-enumerates and is then refused
while being brought up, so it needs a replug. The mount went read-only when
the write failed, which is what a failed write is supposed to do to a mount,
and the file system took no other harm.

Each command carries at most eight sectors (`MSC_MAX_SECTORS`), so a 1 MiB
write is about two hundred and fifty commands; 256 KiB, sixty-four of them,
is enough.

The same stick in the machine's own port, where it enumerates at high speed
(`port 3 high 781:55a9`), takes those writes: 256 KiB, 1 MiB and 8 MiB, each
written to the FAT32 volume and synced, 33 MiB in all, with nothing in the
log and the mount still read-write afterwards. The size is not what decides
it; the path through the hub is.

This is the mass-storage path and not this milestone's code: no loop device
was attached and the buffer cache's chain rules do not reach a device with no
backing. It is recorded here because it was found here, and because the
milestone's remaining row on the machine, a KRFFS volume of the stick,
mounted and written, runs over that path.
