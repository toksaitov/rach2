# `block/`: disk access

The block layer publishes one whole-disk device as `block0`. The legacy ATA driver and the AHCI driver implement the same synchronous, sector-addressed interface. MBR discovery, KRFFS, the page cache, and the raw disk devices use that interface without selecting a driver. The mounted file systems reach it through `block_request`, the one entry that also serves a device that sleeps its caller.

## Block devices and partitions

A `block_device` reports a sector size, a 64-bit sector count, two transfer functions, and private driver data. Rach2 supports 512-byte sectors. A read or write moves a positive number of complete sectors and returns zero or a negative error. Requests that start at or past the device end, cross the end, or contain zero sectors fail.

A device with the `BLOCK_FAULT_SAFE` flag completes its transfer functions by polling, without allocating or scheduling, and may be used with interrupts off. ATA, AHCI and every partition view over them have the flag. It is required by the page-cache window: a supervisor page fault enters through an interrupt gate with interrupts disabled, then may read or evict a 4096-byte page through the block device, so the window requires the flag of its backing device. A fill that cannot be completed causes an unrecoverable page-cache fault. A failed writeback remains dirty and is reported by `pc_sync`.

`block_ready` returns an existing `block0` or calls `block_probe`. A failed probe is not cached. `rach2.nodisk` prevents every probe. With `rach2.disk=ahci`, the kernel tries AHCI and then falls back to ATA. Without that option it tries ATA only.

`block_partition_init` creates a half-open view of a backing device. It adds the partition start to each request and clamps a view that extends past the backing device. A start past the device produces an empty view.

## The block request

`block_request` transfers whole sectors and sleeps the caller when the device does. On a `BLOCK_FAULT_SAFE` device it calls the transfer function and returns, from any context, at boot included. A device without the flag has `submit` instead: `submit` starts one request and returns, and the device reports the end through `block_request_complete` from whatever context it completes in. Such a device is used from syscall context only. The transfer is split into requests of at most eight sectors, each taken from a pool of sixteen with its own page; a write is copied into the page before `submit` and a read is copied out after a successful completion, so no device writes into memory the caller may have reused. The caller sleeps between `submit` and the completion or the device's deadline, five seconds unless the device sets `deadline_ticks`; the timer advances the deadlines once per tick.

A request whose deadline passes answers `EIO` and is quarantined with its page until the device answers late; the late completion frees the slot and is recorded. Each slot has a generation, advanced when the slot is taken and when a late completion frees it, and a completion whose generation is not the slot's current one is dropped with a record. A transfer that finds no free slot answers `EIO`.

`rach2.blocklag=N` wraps the boot disk in the lag device for the partition nodes and the mounts, while the root window keeps the real device. The wrapper has no `BLOCK_FAULT_SAFE` flag; it runs each transfer at submit time, polled, and delivers the completion `N` ticks later from the timer, twice, with the second copy carrying an error, so a kernel that accepted a stale generation would fail a healthy request. With `N` above the deadline the request times out before its completion arrives, which is how the quarantine is exercised by `tests/run mount` without a mutation. `rach2.blockfail=N` beside it makes the Nth write through the wrapper fail with `EIO`, so the buffer cache's failure record and a mount going read-only can be seen in a boot.

## Partition discovery and the disk root

Boot-device discovery reads sector zero once and parses the four MBR entries. The same parsed table is later used for the disk root, so the published raw windows and the mounted file system cannot be based on different reads of the table.

`/dev/grubcore` covers LBA 1 through 2047. It is published when the disk has at least 2048 sectors and no non-empty partition overlaps that range. A partition in the gap suppresses `grubcore` only.

`/dev/bootpart` uses MBR entry 0. The entry must:

* have FAT32 type `0x0b` or `0x0c`;
* start at or after LBA 2048 and contain at least one sector;
* fit within the disk;
* overlap no other non-empty MBR entry; and
* be smaller than 2 GiB, so the 32-bit `lseek` can name every byte.

The disk-root path examines the first sector of each non-empty partition and mounts the first valid KRFFS root. The MBR type byte does not select KRFFS. The chosen partition must fit the disk and the page-cache window, and its sector count must be a multiple of eight. If entry 0 itself contains the mounted root, the kernel withdraws `/dev/bootpart`.

## The GUID partition table

`gpt.c` reads the table a stick formatted on a Mac has and a disk over two terabytes must have. A protective MBR entry of type `0xEE` at index zero says the table is a GUID one; the header is at sector 1 and the entry array is where the header says. The parsing is pure, so `gpt_hosttest` drives it against tables no disk would carry.

The header is judged before anything it says is used: the signature, revision 1.0, a size between 92 and 512 bytes, the CRC32 over the header with its own checksum field taken as zero, and the sector the header says it is on, which is what tells a header read from the backup apart from a copy of the primary left there. Then what it says about the array: at most 128 entries, an entry at least 128 bytes and a multiple of 128, their product at most 16 KiB, a usable range that does not run backwards or past the medium, and an array that is on the medium. The product is bounded before it is taken, because a reader that multiplied the count by the size without looking would compute an offset the stick chose.

The array is read whole and covered by its own CRC32 before a single entry is looked at. A reader that took the first four entries would accept a table whose fifth entry is rubbish, and this kernel publishes four partitions but judges all of them. An entry is used when its type GUID is not all zeros; it is published when its first sector is at or before its last, both lie inside the usable range, its last sector is within 32 bits, which is what a ten-byte read addresses, and it overlaps nothing already taken. An entry that fails is passed over with the rest still read, and how many were passed over is reported, because a table with one bad entry is not a table with no partitions.

## The stick as a disk

`usbdisk.c` turns a stick the class driver has brought up into `/dev/usbN` with its partitions. A stick gets the lowest free number of the two, its first sector is read, and what it holds decides the rest: a protective MBR is read as a GUID table, an MBR signature as the four classic entries, judged here against the medium the same way, and anything else as a volume with no table, which is published as the whole node alone and mounted through it. The whole node is published in every case.

The reading happens in a syscall and not in the pump. A transfer to a stick sleeps its caller and the pump runs in an interrupt, so the class driver records that a stick arrived or went and the next process to enter the kernel does the work, through `usbdisk_service`, which the syscall entry calls before every dispatch.

A stick that is pulled out is hidden from `/dev` at once, so nothing new can open it, and its windows are kept while anything still reads through them. The buffer cache is told: a clean buffer nobody holds is dropped, a dirty one has nowhere to be written and takes the mounts on the device read-only, and one a process holds is marked to be dropped at its release, because a buffer cannot be reused under its holder. Then the stick is a tombstone until nothing holds it. The holds are counted rather than kept: the mounts on its windows and what is open on them, the descriptions on its nodes, the requests in flight or quarantined for it, and the cache buffers held on it. A count that drifted by one would leave a name taken for the life of the machine, and the four things that can hold a stick are all countable where they are.

A stick whose count is zero, which is one nobody opened, is reclaimed as soon as it is pulled: its nodes, its number and the class driver's slot all go back. Nothing of it is reused before then, so a stick plugged in afterwards is a new device with a new claim, which may be given the same number and never the same buffers. `/proc/usb` reports the names free and the sticks given up, so a hundred plugs and pulls can be shown to exhaust nothing.

## ATA

`ata.c` probes the primary legacy channel at ports `0x1f0` and `0x3f6`, selects its master device, and issues `IDENTIFY`. The driver uses LBA28 PIO, so it publishes at most 2^28 sectors (128 GiB). One command transfers at most 256 sectors. Each sector has its own `BSY` and `DRQ` handshake.

The ATA interrupt line remains disabled. Status waits use a fixed iteration limit because the timer interrupt cannot advance while a page fault has interrupts disabled. Each successful write command is followed by `FLUSH CACHE`. The command counter records both the data command and the flush.

## AHCI

`ahci.c` finds the first PCI mass-storage controller in AHCI mode and maps BAR5 as its ABAR. Bring-up requests firmware ownership when supported, resets the HBA, applies the Intel PCS port-enable correction to the supported controller families, and disables HBA and port interrupts. It power-enables and COMRESETs each implemented port, then selects the first port whose link is present, whose device becomes ready, and whose signature is `0x00000101`.

The driver uses one command slot, one Register Host-to-Device FIS, and one PRDT entry. `READ DMA EXT` and `WRITE DMA EXT` use a 48-bit LBA. One command transfers at most 8192 sectors, or 4 MiB. A write is followed by `FLUSH CACHE EXT`. Completion checks the fatal port status, task-file error bits, and the transferred byte count. A failed or timed-out command stops, clears, and restarts the port before returning an error.

AHCI waits are measured with the 8254 counter through `k_udelay`: one second for HBA, link, and command waits, and five seconds for a device to finish COMRESET. The driver still busy-polls completion, so a transfer stalls the single CPU until it completes.

The command list, received-FIS area, command table, and current transfer buffer must have physical addresses below 4 GiB. The transfer buffer must also be physically contiguous. Current callers satisfy this with one direct-mapped page or the static raw-transfer buffer. The driver synchronizes this memory with `CLFLUSH`, or `WBINVD` when `CLFLUSH` is unavailable, around each DMA operation.

Rach2 has no AHCI interrupts, request queue, NCQ, hot-plug, port multiplier, or ATAPI support. It publishes only the first disk as `block0`.

## Raw disk devices

The raw devices are synthetic character devices. Their open-file descriptions use a 64-bit offset, and `_llseek` can address the complete window. Reads and writes require a sector-aligned offset and a byte count divisible by 512. Reads at or past EOF return zero, and a read crossing EOF is shortened. A write that starts past or crosses the end returns `ENOSPC` before writing any sector.

Every transfer uses a page-aligned 32 KiB staging buffer. The loop sends at most 64 sectors to the block device per call. If a later batch fails, the call returns the bytes from earlier completed batches. A failed write batch may have reached the disk in part because the block interface cannot report progress within one command. A zero-length write returns zero. A zero-length read returns zero before any device is called.

Only one write-capable descriptor may be open across `/dev/bootpart`, `/dev/grubcore`, and `/dev/disk0`. Readers may remain open during a write. `/dev/grubcore` additionally permits only one open description at a time. Beside that rule is the claim table: a mount claims its partition's sector range on the backing disk from its constructor to its unmount, and while a range is claimed a write-capable open of a window over it answers `EBUSY`; while such a descriptor is open, a mount on the range answers `EBUSY`. Ranges are compared as ranges, so `/dev/bootpart` and `/dev/disk0p1` naming one partition exclude each other's writer.

The devices have these publication and permission rules:

* `/dev/bootpart` is mode 0600 and appears after its MBR entry passes the checks above.
* `/dev/grubcore` is mode 0600 and appears when the post-MBR gap is safe.
* `/dev/disk0` is mode 0400. `rach2.diskdiag` publishes it after a disk answers; a recovery build requests the same publication without that boot option. Its window is the smaller of the device size and 2^32 sectors, or 2 TiB.
* `/dev/disk0p1` to `/dev/disk0p4` are mode 0400, one per nonempty MBR entry, each the entry's range as a read-only window. They are the sources `mount(2)` takes, with `/dev/bootpart`, and are published with the boot windows and withdrawn with them.

The modes reject unprivileged opens. Effective uid 0 bypasses the mode check, so root can open ordinary `/dev/disk0` with `O_RDWR`. The ordinary kernel's write handler then returns `EPERM`. A recovery kernel compiles the write implementation and checks effective uid 0 again on every write. The bounded boot windows also repeat that effective-uid check when they write.

The debug-shell `ahci` command reports an error and performs no re-probe while KRFFS is mounted from disk. Otherwise it withdraws all three raw devices, initializes AHCI directly, dumps the controller, reads sector zero, and runs discovery again. It republishes `/dev/disk0` only when `rach2.diskdiag` is present; a recovery boot without that option loses the whole-disk node after this manual re-probe.

## Disk policy and recovery authority

Two independent build switches limit writes.

`RACH2_DISK_READONLY` selects the metal kernel's driver policy. The ATA and AHCI issue paths permit only `READ SECTORS`, `READ SECTORS EXT`, `READ DMA EXT`, and `IDENTIFY`; every other command byte returns failure. This also prevents writes through `/dev/bootpart` and `/dev/grubcore`. `tools/inspect/check_diskpolicy.sh` requires exactly one policy banner in the built kernel.

The policy is the two disk drivers' allowlist and covers the disk they drive. A flash drive is written through the USB class driver, which the policy does not reach, so a machine booted from the read-only stick can still write a stick; the disk the policy exists to protect is the one in the machine.

`RACH2_RECOVERY` controls the `/dev/disk0` write handler and automatic whole-disk publication. The ordinary kernel has the `EPERM` body. The recovery kernel has the root-only write body. This switch does not bypass `RACH2_DISK_READONLY`; the Makefile builds the recovery kernel with recovery authority and the normal read-write driver policy. `tools/inspect/check_recovery.sh` checks the authority banner.

The command and flush counters cover attempts issued since boot. `/proc/disk` reports them beside the raw-device batch size and call count.

## Disk information

`/proc/disk` is always a resolvable read-only procfs file. Before `diskinfo_probe` it reports `state not-asked`, the driver name recorded by any earlier probe, the compiled driver policy, and the buffer cache's three counters (`cache-hits`, `cache-fills`, `cache-waits`: reads served from a buffer, reads that filled one, and waits for a buffer another process held), which are printed whatever the disk's state because a mount can be on any device the cache reads. A requested probe reports `no-device` when no disk answers.

For a present disk, `diskinfo_probe` adopts `block0`, reads sector zero, and publishes `/dev/disk0`. The report includes the driver, AHCI port, mount state, sanitized ATA model, serial and firmware strings, sector and window sizes, MBR signature and disk id, four partition entries, and the live raw-call, command, and flush counters. Opening `/proc/disk` generates a snapshot and issues no disk command.

## Verification

`tests/run host` runs the pure block tests under AddressSanitizer and UndefinedBehaviorSanitizer. `block_hosttest` checks partition forwarding and bounds. `mbr_hosttest` checks parsing, overlap, disk-end, gap, and FAT-entry cases. `gpt_hosttest` checks the CRC32 against its published value, the header's every rule, the whole-array checksum, and the entry rules, including an entry array damaged beyond the fourth entry. `usbdisk_hosttest` drives the disk layer against a medium in memory: each table format, each way an entry or a table can be wrong, the numbering, and the reclaiming, which happens at once for a stick nobody held and waits for the last hold otherwise. `diskcmd_hosttest` tests all 256 command bytes under both policies. `diskinfo_hosttest` checks IDENTIFY strings, 64-bit decimal conversion, the 2 TiB window, and the exact procfs record. `pagecache_hosttest` checks fill, eviction, writeback, and failed-write retention above the block interface. `ahci_hosttest` drives the AHCI driver against a model of a controller through `struct ahci_io`, the register interface the driver reads and writes through; each row drives one step of initialization or one command and requires the driver's answer, including a reset that never completes, a port with no device or a device that is not a disk, a command that never completes, and a device that reports a fatal status.

The default `make test` boot has no data disk and does not exercise either disk driver. The disk targets provide that coverage:

* `tests/run disk`, `tests/run disk-ahci`, and `tests/run disk-q35` run `blockprobe` through ATA, a separate AHCI controller, and the q35 controller at PCI function 2.
* `tests/run persist` and `tests/run persist-ahci` write and fsync on one boot, then read the file from the same disk on the next boot.
* `tests/run stress` and `tests/run stress-ahci` write and verify 33 MiB through the 4 MiB resident page cache.
* `tests/run bootpart` checks an absent boot partition, a writable FAT fixture, both bounded devices under a RAM root, and `_llseek` markers at 5 GiB and 258 GiB on a sparse AHCI disk.
* `tests/run authority` compares ordinary and recovery kernels, checks the write exclusion rule, and requires a 200-sector raw transfer to take four 64-sector calls.
* `tests/run diskpolicy` proves that a mount write fails under the read-only artifact and leaves the disk image unchanged. `tests/run diskdiag` compares raw-sector hashes and requires the complete read-only session to leave the image unchanged.

`tests/run grub`, `tests/run menu`, `tests/run install`, `tests/run drill`, and `tests/run sealed` exercise the raw windows in their boot-loader and recovery workflows. The sealed test attaches the boot disk through AHCI and boots it through GRUB without QEMU's `-kernel` option.
