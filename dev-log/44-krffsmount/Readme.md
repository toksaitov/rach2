# Milestone 44: mounting KRFFS

A KRFFS volume that is not the root can be mounted from inside Rach2: a
partition of a disk or of a stick, or an image file on any mounted file
system through a loop device. `mount(2)` takes the type `krffs`, and the
kernel has one implementation of the format rather than two.

## What is built

`krffs.c` addresses every node by its offset from the first byte of the
region and reaches the medium through a backend of three calls, so the root
in memory and a volume on a disk are the same code with three functions
between them. The root keeps the pointer interface its callers use, at the
end of that file. `krffsmount.c` is the other backend, over a block device
through the buffer cache, and the VFS type `mount(2)` places: two volumes at
once, sixteen open files each, the mount's lock around every change, and a
node that moves followed into the descriptions, the working directories, the
covering mounts and the loop devices that named it.

`/dev/loop0` to `/dev/loop3` are block devices over regular files, attached
and detached through `/dev/loop-control` by `loopctl` and listed in
`/proc/loop`. A write to one is a write of the file and a sync of it, so a
block the cache says it has written is a block the file has taken. The block
layer gained a third kind of device for them, one that sleeps its caller and
answers in the same call without a slot of the request pool, and a device
now names the device its bytes live on; the buffer cache reads that chain,
so a file system on a loop device may take a block of the device under it
while it holds one of its own, and an eviction that would write a dirty
block of a device below is passed over rather than deadlocked into.

## What a stop can leave

The order of writes is the milestone's substance and it is written down in
`rach2/sys/fs/krffs.Readme.md`: the forward chain is the truth, the bytes
reach the medium before the field that publishes them, one field publishes
one change, and the backward link is derived and repaired at the next mount.
The check refuses what that order cannot produce and repairs the four things
it can: a backward link the chain contradicts, a reserved node with no name,
one name on two nodes, and the dirty flag. The host's `fsck.krffs` makes the
same judgements, including the one this milestone added, that a directory
never loses its name to a file that took it.

One state the order cannot rule out is a field the medium takes in two
pieces, because a node's header falls where the packing puts it and an
eight-byte field can cross a cache block. The core refuses such a write
instead of making it, comparing the bytes it is about to write with the
bytes that are there and refusing only when the ones that change fall on
both sides of the line.

## What proves it

`tests/run krffsmount` is eleven boots: a volume read to the last byte,
written, read back after a reboot, taken through a machine killed while
writing, repaired on a volume broken on purpose, written by two processes,
refused in three malformed shapes, written by a device that fails, mounted
two at a time with the third refused, and a stick pulled from under a mount
and plugged back in while that mount is still there. `tests/run loopdev` is
seven boots over FAT32, ext2 and KRFFS images, on a disk, on a stick, on an
ext2 volume, on a disk that sleeps its caller, and on the memory root.
`tests/run krffscheck` breaks images on purpose and replays the update
protocol against the offline checker. `krffs_hosttest` runs the core over a
recording backend and replays every prefix of every operation, every lost
cache block and every lost write. `bcache_hosttest` holds the cache to its
rules over a chain of devices.

## The machine

The notebook mounted an image on its ext2 root through a loop device, wrote
it, unmounted it, and the host's checker found it clean with the machine's
bytes in it. A KRFFS partition was then written to the owner's stick from
the mac mini, and the notebook mounted it over a SuperSpeed hub, read it,
wrote a file, a directory and half a megabyte, and unmounted it; the
partition read back here is clean and holds everything the machine wrote.
`hardware.md` has the readings, including the stick's sustained-write fault
recorded in this milestone and not reproduced since.

## Trust

The format's order is proved by a model that replays every stop, not by a
machine that was interrupted: what a real power cut does to a real medium is
outside what any of this can see, and the mount check is what judges an
image afterwards. The loop device is proved over the file systems this tree
has and the devices QEMU models, and once on the owner's stick through his
hub. The refusal of a field the medium would take in two pieces is proved by
rows on a memory image and by reading; no test here interrupts a real disk
between two of its blocks. What the suites prove, they prove on every run.
