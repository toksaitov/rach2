# Milestone 47: storage

## The goal

Any disk on a controller this kernel drives, with a file system it reads,
is usable: found at boot or when plugged in, published as `/dev/diskN`
with a node per partition from an MBR or a GPT, mountable by node, chosen
as root by a stable identity rather than by being first, written raw from
recovery under the same authority whatever its bus, and as large as the
format allows. Two SATA disks, a SATA and an IDE disk, a master and a
slave, three USB sticks: each is a disk like the others. The disk does not
hold the processor while it works. The root can be nearly the whole disk,
on this notebook or another, and a file can be larger than 4 GiB. The
milestone is complete when the proof set below passes in QEMU and the
Lenovo's root has been grown to its disk under the rule.

## What the tree settles before this plan

* One disk. `block0` is global and `block_probe` tries ATA unless
  `rach2.disk=ahci` names AHCI, and then not ATA (`block.c:101`). AHCI
  takes the first matching controller and the first port with a SATA disk
  (`ahci.c:351`, `:646`), with one command list, one received-FIS area,
  one command table and one polled slot with interrupts off (`ahci.c:135`,
  `:166`, `:544`); DMA addresses are pointers cast (`ahci.c:790`). ATA is
  the primary channel's legacy ports and its master, in LBA28, so at most
  128 GiB (`ata.c:14`, `:40`, `:351`).
* One table. `kernel.c:1838` keeps four MBR entries read at boot;
  `devfs.c:969` names `disk0` and `disk0p1` to `p4` literally; `mbr_parse`
  reads the four primary entries and no extended ones (`mbr.c:17`). GPT is
  parsed only for USB sticks (`usbdisk.c:217`), from the primary header
  with no fallback, keeps at most four used entries, and rejects a
  partition ending above LBA `0xFFFFFFFF` (`gpt.h:23`, `gpt.c:186`); the
  partition GUID and label are discarded. `diskinfo` is one record
  (`diskinfo.c:18`) capped at `2^32` sectors.
* One root. KRFFS is the first entry on `block0` with a root node
  (`kernel.c:2012`); `rach2.root=ext2` names a type and takes the first
  ext2 partition (`kernel.c:1921`). Root mounts before USB storage exists
  (`kernel.c:3225`).
* Mounts: `sys_mount` copies a 64-byte source and the record keeps 32
  (`syscall.c:1981`, `vfs.h:235`); device claims have eight entries; the
  raw-write reservation is global across every window (`devfs.c:1103`), so
  two disks would share one writer.
* USB: mounted USB file systems already write through the block layer with
  WRITE(10) (`msc.c:836`); the raw USB nodes answer `EPERM` always,
  recovery included (`devfs.c:561`); MSC and the USB disk layer have two
  slots each (`msc.h:21`, `usbdisk.h:27`); MSC takes one LUN, refuses a
  logical sector that is not 512 bytes and a capacity that needs READ
  CAPACITY(16) (`msc.c:862`, `:966`), and its pending queue of sixteen
  answers `EIO` when full (`msc.c:1256`). The common block code assumes
  512-byte sectors though it has a `sector_size` field (`block.h:7`).
* The file-system checks have their own bounds: a shared bitmap of `2^22`
  blocks (`fatcommon.h:58`), ext2's `2^20` inodes, depth 32 and 512 links
  (`ext2.c:2758`), FAT32's and exFAT's depth and repair records
  (`fat32.c:2385`, `exfat.c:3198`); the ext2 driver refuses more than 128
  block groups, 16 GiB (`ext2.h:9`), and reads the descriptors as one
  array (`ext2.c:93`).
* Offsets: ordinary file offsets are 32 bits (`fd.h:96`) while raw and VFS
  offsets are 64 (`vfs.h:149`); ext2 and exFAT cap a write near 4 GiB
  (`ext2.c:2053`, `exfat.c:2836`); KRFFS turns an offset into a
  pointer-sized handle (`krffsmount.c:70`).
* Tools: the e2fsprogs port builds `e2fsck` only (`build_e2fsprogs.sh:91`);
  `mkext2.sh` takes a size and makes a sparse image; `reflash` writes
  every byte of its stream (`sectorcopy.c:126`); the recovery image has a
  4 MiB budget (`Makefile:493`); recovery's `init` scans exactly the four
  `disk0` names (`init.c:214`). GRUB's embedded configuration and the
  generated menu name `hd0` and fixed partition numbers
  (`mkdisk_grub.sh:215`); the pinned core has `part_msdos` and not
  `part_gpt` (`regen_grub.sh:45`); the boot windows assume entry zero and
  the gap after the MBR (`devfs.c:1627`).
* The Lenovo's SATA disk has 488397168 sectors, 238475 MiB
  (`dev-log/43-machine/hardware.md:43`); its fourth entry is 2 GiB with a
  1 GiB file system in it. The 1 TB disk in that record was a USB disk.
* Milestone 46 gave the kernel a heap, allocated objects, the waiting
  rules and allocated interrupt vectors.

## Decisions

### One registry for every disk

A block device is an instance record: identity (model, serial, the MBR
signature or the GPT disk GUID), capacity, logical sector size, transport,
controller location, operations and a lifetime state. A partition is a
child view over its disk, which `block_partition_t` already is
(`block.h:145`). `/dev/diskN` and `/dev/diskNpM` are published for every
disk in enumeration order with the partition's own entry number; N is a
number for this boot, not an identity. A removed disk's record lives while
a handle still refers to it and a replacement never inherits the handle.
`/proc/disk` lists them all. The raw-write reservation, the claims and
`diskinfo` become per instance.

### Partitions are discovered by one service

MBR with extended partitions, and GPT with its backup header, every
entry, the GUID and the label kept, no 32-bit LBA cap, parsed by one
service for every disk on every bus, with a guarded reread for a disk none
of whose partitions is open or mounted. GPT is for data disks; a BIOS
install on GPT needs a boot partition and a different GRUB core and is not
this milestone's, and the Readme says so.

### The root is chosen by identity

The boot line names the root by identity and the type separately:
`rach2.root=PARTUUID=...` for GPT, `MBRID=signature,entry` for MBR, an
ext2 UUID as an alternative, a label as an alias that fails on ambiguity,
and `/dev/diskNpM` as the rescue override. Two partitions with one
identity are an error, not a first match. Boot, root, fallback and
recovery each have an identity; `bootctl` resolves the running root's
boot partition through it and never assumes the first disk's first entry.
The installer generates fresh identities when it copies the image.

### Every port and channel, both drivers, together

AHCI gets per-controller and per-port state and DMA storage and probes
every supported controller and every port with a disk; ATA gets
per-channel and per-device state, both channels, master and slave, native
PCI ports where the controller has them, and LBA48. Both probe, always.

### The disk is driven by interrupts, one command at a time

AHCI enables interrupts and completes through the request layer's
sleeping path with one active command per port and a software queue above
it, which lets the processor run someone else during a transfer and lets
two disks progress independently. Several commands in flight to one disk
is NCQ, with tags, ordering, flushes and recovery of its own; it is not
this milestone's. The polled path stays for boot and for the recovery
kernel's raw window.

### USB disks are disks

USB disks register through the same service, every usable LUN, with READ
CAPACITY(16) and READ(16)/WRITE(16) where the capacity needs them, as many
as the memory allows, and their raw nodes take recovery's writes under the
same authority and exclusion as SATA. Hardware write protection stays in
force. A queue that is full waits, as milestone 46's rule says.

### Sector size is discovered

The block layer, the partition parser and raw access use the disk's
logical sector size; each file-system driver checks its own assumption
against it and refuses by name. Until the row with a 4096-byte-sector
QEMU disk passes, the restriction is reported, not hidden.

### Large volumes and large files

The ext2 descriptor array is sized at mount from the superblock. The
checks size their workspaces from the volume's geometry and the memory
the machine has; a dirty volume the check cannot hold mounts read-only,
says so, and is repaired by recovery's `e2fsck`; a small fixed limit while
memory is free is not acceptable. Ordinary file offsets become 64 bits so
a file may be as large as the format allows. `resize2fs` is ported and
staged on recovery, its memory measured on a target-sized volume, with
the extent package as the recorded fallback if it does not fit. `growroot`
takes a disk and a partition identity, grows only into free space after
the partition after checking its neighbours, and never means "rewrite
entry four". The limits stated in the Readme are exact: the MBR's 2 TiB,
the largest size tested, and what each check can hold.

## Phases

### Phase 1: the registry

Instances, partitions as children, `/dev/diskN`, `/proc/disk`, claims and
reservation per instance, the existing single-disk boot unchanged. Proof:
the disk suites unchanged; a QEMU boot with two AHCI disks of different
sizes showing both, each partition mountable and writable.

### Phase 2: partitions and identity

The shared discovery service with MBR extended entries and full GPT; the
guarded reread; root by identity; `bootctl` through identity. Proof: rows
with MBR on one disk and GPT with more than four partitions on another, a
damaged primary GPT, duplicate identities refused, a blank disk, a root
whose enumeration number changes between boots and is still found; the
mutation that takes the first match makes the duplicate row fail.

### Phase 3: the drivers

AHCI on every controller and port, interrupt-driven with the software
queue; ATA on both channels, both devices, LBA48; both probed together.
Proof: AHCI and IDE together with each as root in turn; master and slave
on both channels; a row reading from two processes on two disks showing
both progressing; a sparse disk larger than 128 GiB written and read near
its end on IDE.

### Phase 4: USB disks

Through the registry; every LUN; the wide commands; raw writes from
recovery; the queue waiting. Proof: three sticks at once, one pulled
during queued I/O and found gone by name, the others unharmed; a stick
written raw from recovery and verified; a stick larger than 2 TiB in QEMU
addressed near its end.

### Phase 5: sectors, volumes and files

Discovered sector size; the ext2 array sized at mount; the checks sized
from geometry and memory with the read-only answer; 64-bit offsets;
`resize2fs` and `growroot`. Proof: a 4096-byte-sector QEMU disk mounted
and checked; a sparse disk the Lenovo's size grown from 1 GiB to the
partition at 64 MiB of RAM with `resize2fs` and `e2fsck` memory recorded,
written near its end, killed and repaired; a growth interrupted between
the entry and the grow repaired by running again; a 5 GiB file written
and read back on ext2 and exFAT.

### Phase 6: the machine

The Lenovo's root grown to its disk under the rule: the stick booted and
shown to work, every sector the growth changes saved on the host and the
stick, which for a grow is the old partition's extent and the table, since
`resize2fs` rewrites metadata inside the old extent; recovery the saved
default until the grown root has booted and been checked.

### Phase 7: the close

Proof, once and in this order: build from clean; the full set; a boot on
the Lenovo and the SSH capstone; everything that comes up fixed; the whole
milestone's diff to Codex in units of six files, most serious first; every
finding fixed; the full set again. The block, file-system and kernel Readmes and the Readme's "Updating the
installed machine" carry
the facts in the same commits as the code and Codex writes their prose in
the same round. Problems found on the way that this milestone did not set
out to fix are already in `dev-log/50-shortcomings/plan.md` with file and
line, and none is fixed here unless the milestone cannot close without it.
Then notes, the milestone's Readme and the index entry.

## Risks

* A geometry change under a mounted volume is corruption; the reread is
  refused while anything is open, and the row proves it.
* Interrupt-driven AHCI on the Lenovo's controller is new ground; the MSI
  path the network card uses is the model, and a boot switch chooses the
  polled path if it fails.
* A stale handle to a removed disk is a use after free unless the record
  outlives it; the pull row is the proof.
* The check workspaces sized from memory can still refuse a volume on a
  small machine; the answer is read-only and recovery, stated, never a
  silent limit.
