# Synthetic file systems

Rach2 generates `/dev` and `/proc` from kernel state. Neither tree is stored in KRFFS. [synthfs.c](synthfs.c) classifies a canonical absolute path, [devfs.c](devfs.c) supplies devices, and [procfs.c](procfs.c) generates process and system files.

## Path routing

`/dev` and `/proc` are the two synthetic entries of the mount table ([vfs.Readme.md](vfs.Readme.md)). The resolver reaches them by name from the root directory and passes each the remaining path. `synth_rest` resolves `.` and `..` before `synth_classify` fills a `vpath`. A two-component device path other than `/dev/pts` becomes `VNODE_DEV`; devfs checks whether the device exists. The caller checks process and descriptor existence. A `..` in that rest that would climb above the mount answers `VFS_CLIMB` and the resolver continues at the root directory. A KRFFS directory named `dev` or `proc` cannot shadow either mount and cannot be created (`EEXIST`).

Synthetic directories can be opened and used by `chdir` and the `*at` calls. For example, `openat` can open `null` relative to an fd for `/dev`. Directory descriptors and the current working directory store canonical synthetic paths, with `/proc/self` resolved to the numeric pid when necessary. A synthetic node lives for the length of one syscall, in a record taken from the kernel heap; the open that follows a resolution binds the device's own operations onto the descriptor. An `O_PATH` open binds nothing: a directory becomes a synthetic directory descriptor that `getdents64` refuses with `EBADF`, and any other node a descriptor that keeps a copy of the node's `vpath` for `fstat` and `fstatfs`, so a device is not opened and a `/proc` file takes no snapshot.

The namespace is fixed. Creation, removal, rename, and timestamp operations below `/dev` or `/proc` return `EROFS`, `execve` returns `EACCES`, and neither can host a mount point (`mount(2)` answers `EINVAL`). A KRFFS root listing does not add `dev` or `proc`; programs must address those mount paths explicitly. `/dev` enumeration lists `shm`, the tmpfs mount that stands in `/dev` by name ([tmpfs.Readme.md](tmpfs.Readme.md)), as a directory, then the device registry, each device as a character or a block device; it does not list the accessible `pts` subdirectory.

## devfs

`/dev` has mode 0755 and file-system id 5. Opening an existing leaf with `O_CREAT|O_EXCL` returns `EEXIST` before descriptor allocation or special device dispatch, including `/dev/ptmx`. Its registry gives each fixed device a stable inode, a character-device number, and root ownership. Open permissions use the same owner, group, and other bits as KRFFS.

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
| `/dev/disk0` | 240:1 | 0400 | Exposes disk 0 after a diagnostic probe, in recovery, or through USB discovery. |
| `/dev/bootlog` | 240:2 | 0400 | Reads the kernel log's 16 KiB ring from its oldest retained record. |
| `/dev/grubcore` | 240:3 | 0600 | Exposes LBA 1 through 2047 when the MBR leaves that range unused. |
| `/dev/disk0p1` to `/dev/disk0p4` | 240:4 to 240:7 | 0400 | One read-only window per nonempty MBR entry, the source `mount(2)` takes. |
| `/dev/diskN` | 240, the minors [`block/Readme.md`](../block/Readme.md) lists | 0400 | The whole medium of a disk or a stick; a stick's is published by its table scan, from the reading of that table until the record is reclaimed. |
| `/dev/diskNpM` | 240, the same scheme | 0400 | One window per partition the table describes, the source `mount(2)` takes. |
| `/dev/loop0` to `/dev/loop3` | 240:32 to 240:35 | 0400 | A block device over a regular file, and the source `mount(2)` takes for a file system in a file. |
| `/dev/loop-control` | 240:36 | 0600 | Attaches a file to a loop device and detaches one, a command per write. |
| `/dev/screenlog` | 240:38 | 0600 | The lowest-priority record the screen shows: one digit, read or written. |

`null`, `zero`, and `full` seek to offset zero. Random devices, terminals, and pty endpoints are streams and return `ESPIPE`. `/dev/bootlog` uses an ordinary offset and supports seeks relative to the current ring length. All synthetic nodes use stable timestamps from the RTC reading taken during startup, or zero when no RTC reading was accepted.

Pty pairs are allocated from the heap without a fixed count. Each pair receives the lowest index not used by a live pair. Opening `/dev/ptmx` allocates a locked pair; `TIOCSPTLCK` must unlock it before `/dev/pts/N` can open. An allocated slave appears in `/dev/pts`, has device number 136:N, and starts with the opener's effective uid and gid and mode 0620. `chmod` and `chown` may change that slave for a login session. The slave uses the normal terminal discipline. Master writes become slave input, while slave output and echo enter a 4096-byte master ring. The pair is released after both endpoints close.

`/proc/usb` has one line per USB device the kernel has configured: its location, the speed, the vendor and product identifiers, and the class and protocol it was driven as. The location is a path, `port 5` for root port 5 and `port 5.2` for port 2 of the hub on it, one more number for each hub down. A machine that has had a stick has further lines: one per stick naming its node, the two names the drive gave to `INQUIRY`, its size in sectors, how many partitions were published, whether its table is still being read, it is ready or it is closing, with the holds left on it when it is closing, how many `SYNCHRONIZE CACHE` commands it completed, which an unmount, a sync and an fsync each send once, and `ro` when it is write protected; and a last line, `disks N free M reclaimed`, gives how many of the two names are free and how many sticks have been given up since boot. A machine that never had a stick writes neither, so a machine with no USB controller, or one booted with `rach2.nousb`, has the file and it is empty. That is a different answer from the file not being there, and it is the one a program that opens it can act on. A program that counts devices counts the lines that begin with `port`.

A loop device is reached through a mount and nowhere else: its node reads and writes nothing, and is there for `mount(2)` to name. `/dev/loop-control` takes one command per write, `attach N /path`, `attach - /path`, `attach ro N /path` and `detach N`; the node is root's, which is what says who may attach anything, and what the commands mean is in [block/Readme.md](../block/Readme.md). `/proc/loop` has a line per attached device: the device, its size in bytes, whether it takes writes, how many writes it has made and how many it has synced, and the path it was attached by.

Whole-disk and partition nodes report block-device mode in `stat`, on USB and on other transports. Reads accept arbitrary byte ranges: the covering sectors are read and the requested bytes copied out. Writes require whole sectors and are available only in recovery builds. The boot windows remain character devices.

`/dev/screenlog` decides how much of the kernel's own record reaches the screen, and nothing else: the ring, the serial port and netconsole keep every record whatever it says, so `/dev/bootlog` and a capture on the port are unaffected. A read gives the current level as one digit and a newline; a write of one digit sets it, clamped to `KLOG_CRIT` at the low end so a panic can never be hidden and to `KLOG_DEBUG` at the high end. The default is `KLOG_INFO`, which keeps the boot log, since a screen that says nothing while a machine starts cannot be told from a machine that has stopped; what it drops is `KLOG_DEBUG`. `rach2.loglevel=N` on the boot line sets it for a boot being brought up, when the screen is the only instrument there is. It exists because the screen is shared: `rach2-install` talks to a person through it and has no other way to keep the machine's routine notes out of the middle of its questions.

`/dev/bootlog` is always present. It reads the kernel log ring, which contains records but no console text. Whole records are evicted from the oldest end to make room. The offset counts from the oldest retained byte. Records may be evicted between chunk copies, including within one read; the returned bytes are not a snapshot. The os-real `dmesg` script reads this device because Rach2 has no `klogctl` interface.

The framebuffer and mouse use the root paths `/fb` and `/mouse`. They are not devfs entries.

## Raw disk windows

Disk discovery reads and validates sector 0 before publishing either boot window. `/dev/bootpart` represents MBR entry 0 only when all of these conditions hold:

* its type is FAT32 `0x0b` or `0x0c`;
* its start and nonzero length fit within the disk;
* it starts at or after LBA 2048 and overlaps no other nonempty entry;
* its length is less than 2 GiB;
* it is not the partition later selected as the KRFFS root.

`/dev/grubcore` represents the fixed half-open range `[1, 2048)`. It requires a disk large enough for that range and no nonempty partition overlapping it. Sector 0 is excluded because it contains the partition table. Both devices are considered before root selection, so a RAM-root boot can still repair its disk.

`rach2.diskdiag` publishes whole-disk windows for registered disks. Recovery builds publish them without that boot option. USB discovery also publishes each discovered disk's whole-disk window. Partition windows are published separately after a table scan. An ATA or AHCI whole-disk window covers at most 2^32 sectors, or 2 TiB at 512 bytes per sector. A USB whole-disk window covers the full reported sector count; `/proc/disk` reports both the device size and the window size. An ordinary kernel permits reads but its device write handler returns `EPERM`. Only a kernel built with `RACH2_RECOVERY` has the whole-disk write implementation, and that implementation rechecks effective uid 0 for every write.

Boot, whole-disk and partition windows use a 64-bit offset shared by duplicated and inherited descriptors. `_llseek` can address the whole window. The legacy 32-bit `lseek` returns `EOVERFLOW` when it cannot return the resolved offset. A seek need not be sector-aligned. Boot-window reads and all raw writes require offsets and lengths that are multiples of 512; otherwise the transfer returns `EINVAL`. Whole-disk and partition reads accept arbitrary byte ranges.

Reads at or past the end return EOF, and a read crossing the end is shortened. A write at or across the end returns `ENOSPC` before writing any part of the request. Transfers use kernel staging buffers, so a DMA driver never receives a user pointer. Poll reports requested read and write readiness without a wait queue; a transfer may still sleep while acquiring a buffer or completing block I/O. ATA and AHCI flush the device cache after every successful write command. A fully successful raw write to ATA or AHCI is therefore durable when it returns. USB raw writes use ordinary SCSI writes without a device-cache flush and have no such return-time guarantee. If a later batch fails, the short count covers only completed earlier batches; the block interface cannot report how much of the failed batch reached the device.

Each physical disk permits one write-capable open-file description across its raw windows. A second writer on that disk gets `EBUSY`; writers on different disks and readers may coexist. `/dev/grubcore` additionally permits only one open-file description at a time because replacement of the embedded loader is a multi-step transaction. Whole-disk withdrawal clears every registered disk's whole-disk window and leaves partition windows unchanged. Reads through an unpublished whole-disk window return `EIO`. Boot windows are withdrawn separately; their descriptors receive `ENXIO` until the backing is published again.

## procfs

`/proc` has mode 0555 and file-system id 6. Directories have mode 0555, ordinary files have mode 0444, `/proc/disk` has mode 0400, and magic links have mode 0777. Its root listing contains these global entries before the live numeric pid directories:

* `/proc/audio` reports the active audio driver and cached HDA codec capabilities, selected paths, packed and expanded connection lists, and the last requested and programmed rate with format readbacks. Reading it sends no codec verb. The report survives stream shutdown and exists when audio is disabled, with `hda ready 0`; [the audio Readme](../audio/Readme.md) defines the fields and playback limits.
* `/proc/cmdline` is the captured kernel command line followed by a newline.
* `/proc/disk` is the disk diagnostic record, the live command counters, and the buffer cache's four counters. The path always exists. Before a diagnostic probe it reports `state not-asked`; only `/dev/disk0` is conditional on the probe. Opening this file requires root.
* `/proc/uptime` reports total and idle delivered 100 Hz ticks as seconds with two decimal places.
* `/proc/stat` reports busy ticks in the system column, idle ticks, the boot epoch, processes created since boot, and current runnable and blocked counts. Its `intr` line is every interrupt the kernel has routed. Untracked counters are zero.
* `/proc/meminfo` reports managed physical memory as `MemTotal` and free memory as `MemFree`. `Buffers` and `Cached` report the data frames allocated to the buffer cache and page cache, respectively; both caches release frames under memory pressure. `MemAvailable` is the sum of `MemFree`, `Buffers`, and `Cached`. `HeapPages` and `HeapUsed` report the kernel heap's page count and bytes in use. The swap fields are zero.
* `/proc/mounts` reports the root first, the recorded KRFFS source and options or, for a root of another type, its device, type and `rw,relatime` or `ro,relatime` from the mount's flags, then the devfs and procfs rows, then `tmpfs /dev/shm tmpfs rw,nosuid,nodev`, and then every mount placed by `mount(2)`, in the six-field `getmntent` form with the flags as words.
* `/proc/filesystems` lists `nodev devfs`, `nodev proc`, and the types `mount(2)` accepts: `vfat`, `exfat`, `ext2`, `krffs`, and `nodev tmpfs`.
* `/proc/console` is the serial receive path's counters, `name value` per line, in the order the path runs: `uart-bytes`, `uart-overruns`, `irq-drains`, `tick-drains`, `reader-drains`, `ring-full`, `ring-max`, `shell-bytes`, `line-drops`, `queue-drops`, `flush-bytes`, `partial-moved`, `read-bytes`, `queue-max`, `line-max`. What each counts is in the kernel Readme's Input section.
* `/proc/interrupts` has one line per vector that has taken an interrupt: the vector, its count, `PIC` for the sixteen vectors the 8259 pair delivers or `MSI` for a device message vector allocated from 48 through 63, and the registered handler names, separated by commas where a line is shared. Message vectors are allocated in order as drivers enable messages, so identify a device by its handler name rather than a fixed vector number. A vector whose handlers registered no name at all prints `none`. A vector that has taken nothing has no line. The last two lines are counts that belong to no vector: `SPU` is the spurious interrupts the local APIC has raised, and `FBK` is the times the USB timer's pump found work that no message was coming for.
* `/proc/net` contains `dev`, `route`, `arp`, and `tcp`. There is no `tcp6` because the kernel has no IPv6 stack.
* `/proc/self` is a magic symlink to the caller's numeric pid.

The network files are snapshots taken at open of the entries then in the kernel tables. `/proc/net/dev` prints the sixteen columns Linux prints, because programs find their number by counting fields. Seven values come from the interface: bytes and frames each way, receive errors, receive drops and transmit drops. Driver counters add missed frames to receive drops and supply receive alignment errors and multicast frames, transmit errors, FIFO underruns, collisions and aborted transmissions. These hardware counters are zero when the driver supplies none. Receive FIFO and both compressed columns are always zero. `/proc/net/route` prints the on-link route and an optional default gateway. `/proc/net/arp` prints the eight-entry address cache. `/proc/net/tcp` prints every TCP control block with Linux state numbers, queue lengths, uid, and the same socket inode used by `fstat` and `/proc/<pid>/fd/N`.

`/proc` lists a process once, by its first thread, whose id is the process id; `/proc/<tid>` of another thread still opens, and `/proc/self` is the calling thread's process. Each live `/proc/<pid>` directory contains `stat`, `cmdline`, `fd`, `maps`, `status`, `statm`, `auxv`, `environ`, `comm`, `mem`, `exe` and `task`:

* `stat` emits fields 1 through 24. Pid, program name, parent, process group, and session come from the process table, and so do the user and system times of all the process's threads and the reaped children's, in ticks, as `times` reports them, or in a thread's `stat` under `task` the thread's own user and system times, the nice value `setpriority` keeps, with the priority Linux reports for it, 20 more, and the thread count. Running and ready processes use state `R`, blocked processes use `S`, zombies use `Z`, and other states also use `R`. Terminal, fault, start-time, address-space, and resident-size fields that the kernel does not track are fixed values or zero.
* `cmdline` has the arguments as the program's initial stack has them, each ended by a NUL, read through its address space, so a program that rewrote them shows what it wrote; a process with no address space shows its program name, at most 15 bytes, as the one argument. The name is inherited by `fork` and replaced by `execve` and by `prctl(PR_SET_NAME)`; `comm` has it with a newline.
* `maps` has a line for each region in Linux's form: the range, the rights with `p` or `s`, the file offset, a device and inode of 0, which the kernel does not keep, and from column 73 the path: the file a region maps, `[heap]`, `[stack]`, or the program for the segments of its image. GNU libc's `pthread_getattr_np` finds the main thread's stack by its `[stack]` line.
* `status` has the lines of Linux's that programs read: the name, umask, state, the thread group, thread and parent ids, the tracer, the four user and group ids, the descriptor table's size, the groups, the memory sizes in kilobytes, the thread count, the signal sets, full capability sets for effective user ID 0 and empty ones otherwise, and `NoNewPrivs`. `statm` has the sizes in pages.
* `auxv` has the auxiliary vector the program was given, through its `AT_NULL` pair, and `environ` the environment strings of its initial stack; either needs effective user ID 0 or the process's real user ID.
* `mem` is the process's memory, not a snapshot: a read or a write at the descriptor's offset, which is the address, goes through the process's address space as a tracer's does (`paging_access`), a page at a time, and a region no access is allowed to is `EIO`; a transfer that runs into one returns the bytes it did before it and moves the offset past them. Opening it needs effective user ID 0, or the process's real user ID and a dumpable process. The descriptor keeps a reference to the address space those checks allowed (`mm` in its `procfs_file`), and a read or a write reaches it only while the process still runs in it: after an `execve`, or once the process ends, it returns 0 and the reference goes, so a program executed later, a set-user-ID one among them, is out of reach of a descriptor opened before.
* `exe` is a link to the program the process runs, by the path its `execve` found it at (`mm->exe`); reading it needs effective user ID 0 or the process's real user ID. The kernel keeps the path and no reference to the file, so a rename or a removal of the program leaves the link naming the path, where Linux follows the file.
* `task` lists the process's threads by their ids, the leader first. `/proc/<pid>/task/<tid>` is classified as the process's directory with the thread's id beside the process's (`tid` in the `vpath`, from `synth_classify_pid`), and `procfs_resolve_id` finds it only when the process exists and the thread is one the directory lists. It has the files of `/proc/<tid>`, the thread's own where a thread has any, and no `task` of its own. A descriptor on it or on its `fd` keeps both ids (`tid` in its `synth_dir` record), so its listing, its `/proc/PID/fd` link and a name relative to it stay under `task`. A path is split into one component more than the deepest one, `/proc/<pid>/task/<tid>/fd/<n>`, so a deeper path is refused rather than read as a shorter one.
* `fd` lists the process's open descriptors as magic links. Targets include KRFFS paths, `/dev` paths, pty endpoints, synthetic directories, `pipe:[inode]`, `socket:[inode]`, `anon_inode:NAME`, and `/memfd:NAME (deleted)` for a file of `memfd_create`.

Following `/proc/self` produces the numeric pid directory. `/proc/<pid>/exe` is the one link the resolver follows as it follows a link of a mounted type: the procfs type reports it as a link and reads its target (`synth_readlink` in `vfs.c`), so `open`, `stat` and `execve` of `/proc/self/exe` reach the file at the program's path, which is the program until it is renamed or removed, and `readlink` and `lstat` the link. Following `/proc/<pid>/fd/N` produces the target descriptor's stat identity, with a description on a mount held for the length of the stat, whose getattr may sleep while the process closes the descriptor; `lstat` sees the link itself. A descriptor that is closed has no link, as it has no entry in the `fd` listing, also while its release waits for another thread's call. This is the identity check used by musl's `ttyname` implementation. `open` of `/proc/<pid>/fd/N` answers `ENOENT`: the link is not reopened as the descriptor it names. Under `O_NOFOLLOW` either link answers `ELOOP`, and under `O_CREAT|O_EXCL` it answers `EEXIST`, as a link on a mounted type does. With `O_PATH`, `O_NOFOLLOW` gives a descriptor on either link, `/proc/self` without it the process directory, and `/proc/<pid>/fd/N` without it `ENOENT` again.

## Snapshot lifetime

Opening a proc text file builds it once in a buffer allocated from the heap. The buffer grows as text is appended, so a report includes every row. If the heap cannot supply the text buffer, open returns `ENOMEM` and logs one line identifying the file. `/proc/disk` alone uses a fixed formatter buffer of `DISKINFO_RECORD_BYTES + DISKINFO_DISKS_BYTES`: 1024 bytes for the main record plus space for every disk and partition record. The current bound is 15232 bytes, including the terminating NUL. Open rejects a result at or above that bound minus one with `EOVERFLOW` and logs the file name.

The descriptor reads this fixed image until EOF. Duplicated and inherited descriptors share the open-file description and its offset; the snapshot and its buffer are freed on the last close of the description. A per-process snapshot therefore remains readable after that process exits. Proc text files are read-only, poll as readable, and report `st_size` as zero because their size is not generated by stat.

Every synthetic directory includes `.` and `..`. `/proc` enumerates live processes, `/proc/<pid>/fd` enumerates live descriptors, and `/dev/pts` enumerates allocated slaves. These listings are snapshots only at the syscall level: another `getdents` call reads the current kernel tables from its saved index.

## Verification

The default `make test` boot runs these relevant probes:

* [devprobe.c](../../../tests/devprobe.c) checks the memory devices, trusted `urandom`, a disk ioctl refused on `/dev/null`, the inode `fstat` and the `/dev` listing give `/dev/null`, `/proc/self/stat`, and the console fd link.
* [ptyprobe.c](../../../tests/ptyprobe.c) checks master allocation, slave unlocking, terminal ioctls, cooked input, echo, and output.
* [procprobe.c](../../../tests/procprobe.c) checks snapshot lifetime, process names, system files, delivered tick accounting, mount records, network formats, socket identity, and complete output for sixteen TCP listeners.
* [statmusl.c](../../../tests/musl/statmusl.c) checks stable synthetic timestamps through path and descriptor stat calls.

[devtest.c](../../../tests/musl/devtest.c) is built and staged as `/musl/devtest`, but the default runner does not invoke it. Its manual checks include `ttyname`, directory enumeration, access and symlink semantics, and `openat` relative to `/dev`.

Specialized targets cover state that one shared test boot cannot provide:

* `tests/run bootpart` uses four boots to check an absent boot partition, a writable fixture, boot devices under a RAM root, and 64-bit whole-disk offsets on a sparse AHCI disk.
* `tests/run authority` compares ordinary and recovery kernels, the one-writer reservation, 64-sector batching, and driver command and cache-flush counts.
* `tests/run procfiles` checks three root-mount sources, then BusyBox `ps`, `df`, `free`, `uptime`, `dmesg`, `route`, `arp`, and `netstat` against the generated files.
* `tests/run host` runs [synthfs_hosttest.c](../../../tests/host/synthfs_hosttest.c) for the core path classifier and [mbr_hosttest.c](../../../tests/host/mbr_hosttest.c) for MBR parsing and boot-window selection.
