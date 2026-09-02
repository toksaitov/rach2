# Synthetic file systems

Rach2 generates `/dev` and `/proc` from kernel state. Neither tree is stored in KRFFS. [synthfs.c](synthfs.c) classifies a canonical absolute path, [devfs.c](devfs.c) supplies devices, and [procfs.c](procfs.c) generates process and system files.

## Path routing

`/dev` and `/proc` are the two synthetic entries of the mount table ([vfs.Readme.md](vfs.Readme.md)). The resolver reaches them by name from the root directory, since the root file system has no node for them, and hands each the rest of the path in one call, which `synth_classify` turns into a `vpath`: a device, a proc file, a directory, or `ENOENT` or `ENOTDIR`. A `..` in that rest that would climb above the mount answers `VFS_CLIMB` and the resolver continues at the root directory. A KRFFS directory named `dev` or `proc` cannot shadow either mount and cannot be created (`EEXIST`).

Synthetic directories can be opened and used by `chdir` and the `*at` calls. For example, `openat` can open `null` relative to an fd for `/dev`. Directory descriptors and the current working directory store canonical synthetic paths, with `/proc/self` resolved to the numeric pid when necessary. A synthetic node lives for the length of one syscall in a pool of sixteen; the open that follows a resolution binds the device's own operations onto the descriptor.

The namespace is fixed. Creation, removal, rename, and timestamp operations below `/dev` or `/proc` return `EROFS`, `execve` returns `EACCES`, and neither can host a mount point (`mount(2)` answers `EINVAL`). A KRFFS root listing does not add `dev` or `proc`; programs must address those mount paths explicitly. `/dev` enumeration lists the device registry but does not list its accessible `pts` subdirectory.

## devfs

`/dev` has mode 0755 and file-system id 5. Its registry gives each fixed device a stable inode, a character-device number, and root ownership. Open permissions use the same owner, group, and other bits as KRFFS.

| Path | Device | Mode | Contract |
|------|--------|------|----------|
| `/dev/null` | 1:3 | 0666 | Reads return EOF; writes are discarded. |
| `/dev/zero` | 1:5 | 0666 | Reads return zero bytes; writes are discarded. |
| `/dev/full` | 1:7 | 0666 | Reads return zero bytes; writes fail with `ENOSPC`. |
| `/dev/random` | 1:8 | 0666 | Reads use the trusted CSPRNG and fail with `EIO` when the seed is untrusted. |
| `/dev/urandom` | 1:9 | 0666 | Uses the same read and trust policy as `/dev/random`. |
| `/dev/console` | 5:1 | 0666 | Opens the kernel console terminal. |
| `/dev/tty` | 5:0 | 0666 | Opens the caller's controlling console or pty slave; a caller without one gets `ENXIO`. |
| `/dev/ptmx` | 5:2 | 0666 | Allocates a pty pair and returns its master. |
| `/dev/bootpart` | 240:0 | 0600 | Exposes a validated FAT boot partition as a raw sector window when one exists. |
| `/dev/disk0` | 240:1 | 0400 | Exposes the diagnostic whole-disk window after a diagnostic probe. |
| `/dev/bootlog` | 240:2 | 0400 | Reads the kernel log's 16 KiB ring from its oldest retained record. |
| `/dev/grubcore` | 240:3 | 0600 | Exposes LBA 1 through 2047 when the MBR leaves that range unused. |
| `/dev/disk0p1` to `/dev/disk0p4` | 240:4 to 240:7 | 0400 | One read-only window per nonempty MBR entry, the source `mount(2)` takes. |
| `/dev/usb0` and `/dev/usb1` | 240:16, 240:21 | 0400 | The whole medium of a stick, present from the reading of its table until it is reclaimed. |
| `/dev/usb0p1` to `/dev/usb1p4` | 240:17 to 240:25 | 0400 | One window per partition the stick's table describes. |

`null`, `zero`, and `full` seek to offset zero. Random devices, terminals, and pty endpoints are streams and return `ESPIPE`. `/dev/bootlog` uses an ordinary offset and supports seeks relative to the current ring length. All synthetic nodes use stable timestamps from the RTC reading taken during startup, or zero when no RTC reading was accepted.

The pty pool has 16 pairs. Opening `/dev/ptmx` allocates a locked pair; `TIOCSPTLCK` must unlock it before `/dev/pts/N` can open. An allocated slave appears in `/dev/pts`, has device number 136:N, and starts with the opener's effective uid and gid and mode 0620. `chmod` and `chown` may change that slave for a login session. The slave uses the normal terminal discipline. Master writes become slave input, while slave output and echo enter a 4096-byte master ring. The pair is released after both endpoints close.

`/proc/usb` has one line per USB device the kernel has configured: its location, the speed, the vendor and product identifiers, and the class and protocol it was driven as. The location is a path, `port 5` for root port 5 and `port 5.2` for port 2 of the hub on it, one more number for each hub down. A machine that has had a stick has further lines: one per stick naming its node, the two names the drive gave to `INQUIRY`, its size in sectors, how many partitions were published, whether its table is still being read, it is ready or it is closing, with the holds left on it when it is closing, how many `SYNCHRONIZE CACHE` commands it completed, which an unmount, a sync and an fsync each send once, and `ro` when it is write protected; and a last line, `disks N free M reclaimed`, gives how many of the two names are free and how many sticks have been given up since boot. A machine that never had a stick writes neither, so a machine with no USB controller, or one booted with `rach2.nousb`, has the file and it is empty. That is a different answer from the file not being there, and it is the one a program that opens it can act on. A program that counts devices counts the lines that begin with `port`.

The stick nodes are block devices in `stat`, and the disk's windows are character devices. That is what `blkid`, `findfs` and `mount LABEL=` walk `/dev` for, so a volume on a stick is found by its label rather than by the order the sticks were plugged in. The nodes are read-only: a stick is written through a file system mounted on it. Unlike the disk's windows, they read any byte range, because `blkid`'s probers ask for the size of a superblock structure at whatever offset it lives and a block device node on Linux answers that; the covering sectors are read and the bytes asked for copied out.

`/dev/bootlog` is always present. It hands out the kernel log's ring, which holds records and not console text: whole records are dropped from the oldest end to make room, so what comes back never begins mid-line. The os-real `dmesg` script reads this device because Rach2 has no `klogctl` interface.

The framebuffer and mouse use the root paths `/fb` and `/mouse`. They are not devfs entries.

## Raw disk windows

Disk discovery reads and validates sector 0 before publishing either boot window. `/dev/bootpart` represents MBR entry 0 only when all of these conditions hold:

* its type is FAT32 `0x0b` or `0x0c`;
* its start and nonzero length fit within the disk;
* it starts at or after LBA 2048 and overlaps no other nonempty entry;
* its length is less than 2 GiB;
* it is not the partition later selected as the KRFFS root.

`/dev/grubcore` represents the fixed half-open range `[1, 2048)`. It requires a disk large enough for that range and no nonempty partition overlapping it. Sector 0 is excluded because it contains the partition table. Both devices are considered before root selection, so a RAM-root boot can still repair its disk.

`/dev/disk0` is published when `rach2.diskdiag` runs the disk diagnostic. A recovery build publishes it without that boot option. The window covers the device up to 2^32 sectors, or 2 TiB at 512 bytes per sector; `/proc/disk` reports both the device size and the window size. An ordinary kernel permits reads but its device write handler returns `EPERM`. Only a kernel built with `RACH2_RECOVERY` has the whole-disk write implementation, and that implementation rechecks effective uid 0 for every write.

All three raw devices use a 64-bit offset shared by duplicated and inherited descriptors. `_llseek` can address the whole window. The legacy 32-bit `lseek` returns `EOVERFLOW` when it cannot return the resolved offset. A seek need not be sector-aligned, but each read and write offset and byte count must be a multiple of 512 or the transfer returns `EINVAL`.

Reads at or past the end return EOF, and a read crossing the end is shortened. A write at or across the end returns `ENOSPC` before writing any part of the request. Transfers pass through a 32 KiB kernel buffer in batches of at most 64 sectors, so a DMA driver never receives a user pointer. ATA and AHCI flush the device cache after every successful write command. A fully successful write is therefore durable when it returns. If a later batch fails, the short count covers only completed earlier batches; the block interface cannot report how much of the failed batch reached the device.

Only one open-file description may hold write access across the three raw windows. A second writer gets `EBUSY`; readers may coexist with it. `/dev/grubcore` additionally permits only one open-file description at a time because replacement of the embedded loader is a multi-step transaction. A controller reprobe withdraws all three windows. Existing descriptors then receive `ENXIO` until the backing is published again.

## procfs

`/proc` has mode 0555 and file-system id 6. Directories have mode 0555, ordinary files have mode 0444, `/proc/disk` has mode 0400, and magic links have mode 0777. Its root listing contains these global entries before the live numeric pid directories:

* `/proc/cmdline` is the captured kernel command line followed by a newline.
* `/proc/disk` is the disk diagnostic record, the live command counters, and the buffer cache's three counters. The path always exists. Before a diagnostic probe it reports `state not-asked`; only `/dev/disk0` is conditional on the probe. Opening this file requires root.
* `/proc/uptime` reports total and idle delivered 100 Hz ticks as seconds with two decimal places.
* `/proc/stat` reports busy ticks in the system column, idle ticks, the boot epoch, processes created since boot, and current runnable and blocked counts. Its `intr` line is every interrupt the kernel has routed. Untracked counters are zero.
* `/proc/meminfo` reports the physical frame pool. Buffers, cache, and swap fields are zero.
* `/proc/mounts` reports the root first, the recorded KRFFS source and options or, for a root of another type, its device, type and `rw,relatime` or `ro,relatime` from the mount's flags, then the devfs and procfs rows, and then every mount placed by `mount(2)`, in the six-field `getmntent` form with the flags as words.
* `/proc/filesystems` lists `nodev devfs`, `nodev proc`, and every type `mount(2)` accepts. KRFFS is absent because the root is not placed through `mount(2)`.
* `/proc/interrupts` has one line per vector that has taken an interrupt: the vector, its count, `PIC` for the sixteen the 8259 pair delivers or `MSI` for the sixteen a PCI device's messages arrive on, and the names the handlers on it registered under, separated by commas where a line is shared. A vector whose handlers registered no name at all prints `none`. A vector that has taken nothing has no line. The last two lines are counts that belong to no vector: `SPU` is the spurious interrupts the local APIC has raised, and `FBK` is the times the USB timer's pump found work that no message was coming for.
* `/proc/net` contains `dev`, `route`, `arp`, and `tcp`. There is no `tcp6` because the kernel has no IPv6 stack.
* `/proc/self` is a magic symlink to the caller's numeric pid.

The network files are snapshots of fixed kernel tables. `/proc/net/dev` prints the sixteen columns Linux prints, because programs find their number by counting fields. Six carry a value: bytes and frames each way, receive errors, receive drops, and transmit drops. The rest are zero. `/proc/net/route` prints the on-link route and an optional default gateway. `/proc/net/arp` prints the eight-entry address cache. `/proc/net/tcp` prints the 16-entry TCP pool with Linux state numbers, queue lengths, uid, and the same socket inode used by `fstat` and `/proc/<pid>/fd/N`.

Each live `/proc/<pid>` directory contains `stat`, `cmdline`, and `fd`:

* `stat` emits fields 1 through 24. Pid, program name, parent, process group, and session come from the process table. Running and ready processes use state `R`, blocked processes use `S`, zombies use `Z`, and other states also use `R`. Terminal, fault, CPU-time, start-time, address-space, and resident-size fields that the kernel does not track are fixed values or zero.
* `cmdline` contains one NUL-terminated argument: the stored program basename. The kernel retains no full argument vector. The basename is limited to 15 bytes, is inherited by `fork`, and is replaced by `execve`.
* `fd` lists the process's open descriptors as magic links. Targets include KRFFS paths, `/dev` paths, pty endpoints, synthetic directories, `pipe:[inode]`, and `socket:[inode]`.

Following `/proc/self` produces the numeric pid directory. Following `/proc/<pid>/fd/N` produces the target descriptor's stat identity; `lstat` sees the link itself. This is the identity check used by musl's `ttyname` implementation. `open` of `/proc/<pid>/fd/N` answers `ENOENT`: the link is not reopened as the descriptor it names. Under `O_NOFOLLOW` either link answers `ELOOP`, and under `O_CREAT|O_EXCL` it answers `EEXIST`, as a link on a mounted type does.

## Snapshot lifetime

Opening a proc text file builds it once in one of eight pooled 3072-byte buffers. Every generator has a declared bound no larger than that buffer. If the pool is full, open returns `ENFILE`. If a generator exceeds its bound, open logs the file name and returns `EOVERFLOW` instead of exposing truncated text.

The descriptor reads this fixed image until EOF. Duplicated and inherited descriptors share the open-file description and its offset; the pool slot is released on the last close. A per-process snapshot therefore remains readable after that process exits. Proc text files are read-only, poll as readable, and report `st_size` as zero because their size is not generated by stat.

Every synthetic directory includes `.` and `..`. `/proc` enumerates live processes, `/proc/<pid>/fd` enumerates live descriptors, and `/dev/pts` enumerates allocated slaves. These listings are snapshots only at the syscall level: another `getdents` call reads the current kernel tables from its saved index.

## Verification

The default `make test` boot runs these relevant probes:

* [devprobe.c](../../../tests/devprobe.c) checks the memory devices, trusted `urandom`, `/proc/self/stat`, and the console fd link.
* [ptyprobe.c](../../../tests/ptyprobe.c) checks master allocation, slave unlocking, terminal ioctls, cooked input, echo, and output.
* [procprobe.c](../../../tests/procprobe.c) checks snapshot lifetime and bounds, process names, system files, delivered tick accounting, mount records, network formats, socket identity, and full TCP-pool output.
* [statmusl.c](../../../tests/musl/statmusl.c) checks stable synthetic timestamps through path and descriptor stat calls.

[devtest.c](../../../tests/musl/devtest.c) is built and staged as `/musl/devtest`, but the default runner does not invoke it. Its manual checks include `ttyname`, directory enumeration, access and symlink semantics, and `openat` relative to `/dev`.

Specialized targets cover state that one shared test boot cannot provide:

* `tests/run bootpart` uses four boots to check an absent boot partition, a writable fixture, boot devices under a RAM root, and 64-bit whole-disk offsets on a sparse AHCI disk.
* `tests/run authority` compares ordinary and recovery kernels, the one-writer reservation, 64-sector batching, and driver command and cache-flush counts.
* `tests/run procfiles` checks three root-mount sources, then BusyBox `ps`, `df`, `free`, `uptime`, `dmesg`, `route`, `arp`, and `netstat` against the generated files.
* `tests/run host` runs [synthfs_hosttest.c](../../../tests/host/synthfs_hosttest.c) for the core path classifier and [mbr_hosttest.c](../../../tests/host/mbr_hosttest.c) for MBR parsing and boot-window selection.
