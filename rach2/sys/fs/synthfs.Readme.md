# Synthetic file systems: `/dev` and `/proc`

Not every path names a file on disk. `/dev/null` is a kernel sink; `/proc/self/fd/0` is a window onto a live process. These are synthetic file systems: the names are backed by in-kernel drivers and process state, generated on demand, never stored in KRFFS. Rach2 mounts two, a `/dev` (`devfs`) and a minimal `/proc` (`procfs`), the slice musl and busybox actually read.

## One classifier, many syscalls

The temptation is to scatter `if (path starts with "/dev")` checks in front of every `krffs_resolve`. That rots. Instead a single pure classifier, `synth_classify()` in [synthfs.c](synthfs.c), turns a path into a `vnode_kind`:

```
""                  -> VNODE_KRFFS        (the real root; resolve via KRFFS)
"dev"               -> VNODE_DEV_DIR
"dev/null"          -> VNODE_DEV          (name = "null")
"proc"              -> VNODE_PROC_DIR
"proc/cmdline"      -> VNODE_PROC_CMDLINE (the boot command line)
"proc/self"         -> VNODE_PROC_PID     (pid = SELF)
"proc/7/stat"       -> VNODE_PROC_FILE
"proc/7/fd"         -> VNODE_PROC_FD_DIR
"proc/self/fd/0"    -> VNODE_PROC_FD_LINK (a magic symlink)
"hello.txt"         -> VNODE_KRFFS
```

Every path-taking syscall (`open`, `stat`, `fstatat`, `readlink`, `getdents`, `mkdir`/`unlink`/`rename`, and the rest) calls it once: a `VNODE_KRFFS` result means "ordinary file, hand it to KRFFS", anything else routes to the devfs and procfs glue. The classifier is pure string logic, so the fence-post-prone component parsing runs natively under ASan in `tests/host/synthfs_hosttest.c`.

## devfs: character devices over `file_ops`

A small registry maps a leaf name to a character device: a `file_ops` vtable plus a `{major, minor}` and a mode. `open("/dev/<name>")` checks the caller's credentials against that mode with the same nine-bit rule the file systems use (every entry is owned root:root), then binds the vtable onto the descriptor (`FD_TYPE_DEVICE`, `priv` is the registry entry), after which the generic read, write, and close paths dispatch through it, so the device code lives nowhere in the syscall layer. The set, with the real Linux device numbers:

| device             | major:minor | mode | behavior                                                                 |
|--------------------|-------------|------|--------------------------------------------------------------------------|
| `null`             | 1:3         | 0666 | read returns EOF, write is discarded                                     |
| `zero`             | 1:5         | 0666 | read returns zeros, write is discarded                                   |
| `full`             | 1:7         | 0666 | read returns zeros, write returns `-ENOSPC`                              |
| `random`/`urandom` | 1:8 / 1:9   | 0666 | read returns the trusted CSPRNG, else `-EIO` (see the krandom Readme)    |
| `console`          | 5:1         | 0666 | the kernel console (`FD_TYPE_CONSOLE`)                                   |
| `tty`              | 5:0         | 0666 | the caller's controlling terminal: the console, a pty slave, or `-ENXIO` |
| `ptmx`             | 5:2         | 0666 | a clone device: each open mints a new pty pair and returns the master    |
| `bootpart`         | 240:0       | 0600 | the boot partition as a bounded raw window (below); root only            |
| `grubcore`         | 240:3       | 0600 | the post-MBR gap, LBA 1 through 2047, as a bounded raw window; root only |

`/dev/bootpart` and `/dev/grubcore` are the two windows a machine needs to manage its own boot without mounting anything, and both are published by disk discovery before any root is chosen rather than at the end of a successful mount. That ordering is the point: a machine booting a RAM image mounts nothing from its disk and is exactly the machine that has to repair the boot partition which stopped it booting.

Nothing is published until sector 0 has been read and its signature accepted. `/dev/grubcore` then covers LBA 1 through 2047, a constant of the disk layout rather than anything the table reports, so a damaged table cannot widen it; it is withheld when any non-empty partition entry reaches into that range, because a disk whose boot loader shares sectors with a file system is one where writing GRUB destroys the file system. Sector 0 is outside the window on purpose: it carries the partition table, and a disk writes whole sectors, so the eight bytes of boot code pointing at the core image cannot be touched without rewriting the table beside them. `/dev/bootpart` needs entry 0 judged against the whole table (`mbr_bootpart_ok` in `sys/block/mbr.c`): FAT-typed, non-empty, inside the disk, starting at or after the gap, and sharing no sector with another entry. The mount adds the one fact discovery cannot know, which entry holds the root, and withdraws the device if entry 0 turns out to be it.

`/dev/disk0`, the whole-disk diagnostic window, is the third of these raw nodes and the only one still gated behind `rach2.diskdiag`; anything that re-initialises a controller withdraws all three together, because each holds a pointer to the device it reads through.

Both boot windows share one implementation. They read and write whole 512-byte sectors at sector-aligned offsets (`-EINVAL` otherwise), through a kernel bounce buffer, because the AHCI driver programs physical DMA addresses and must never see a user pointer; a read at the end returns 0, a write at or past it `-ENOSPC`, and both disk drivers flush the drive cache per write, so a returned write is durable. Each seeks like a file whose size is its window's. The write path checks the effective uid rather than trusting the open: the mode stops a non-root caller at open and nothing else, since the credential check passes uid 0 whatever the mode says, so a process could otherwise open one of these while privileged, drop privilege, and keep writing through the descriptor.

`lseek` on the mem devices returns 0 (Linux's `null_lseek`); the stream and terminal devices are `-ESPIPE` (a valid fd, just not seekable). `stat` and `fstat` report a character device (`S_IFCHR`) with the entry's mode and a synthetic `st_dev` distinct from KRFFS's, so a device's `(st_dev, st_ino)` identity never aliases a real file's.

## procfs: process state as files

Generated per query from the process table:

* `/proc/<pid>/stat`: the space-separated fields ps parses (pid, `(comm)`, state, ppid, and the rest). Accounting fields the kernel does not track are zero, but the shape is exact and the `comm` parentheses are scrubbed so a naive parser is not fooled.
* `/proc/<pid>/cmdline`: the program name (the kernel keeps `comm`, not the full argv).
* `/proc/<pid>/fd/<n>`: a magic symlink whose `readlink` returns the descriptor's path (`/dev/console` for the console, `/dev/<name>` for a device, and so on).
* `/proc/self` is the caller; `getdents("/proc")` lists `cmdline`, `self`, and the live PIDs.
* `/proc/cmdline`: the boot command line the loader passed, plus a newline, a global file beside the pid tree. It is how a program learns which GRUB menu entry booted the machine (the entries carry `rach2.slot=` tokens).

A `/proc` text file is snapshotted into a pooled buffer at open and served from there, so a read sees one consistent moment and dup and fork share the snapshot until the last descriptor closes (the open-file-description lifetime, freed by the `file_ops` close hook). Text is built with bounded append helpers and hand-rolled integer formatting: no `snprintf`, no 64-bit division (the kernel has no libgcc).

## What `ttyname(0)` exercises

`ttyname` is the integration target, and it pulls the whole thing together. musl's `ttyname_r` (verified against the vendored `musl-1.2.5`) issues `isatty(fd)` as `ioctl(TIOCGWINSZ)`, which the console answers (so it reads as a terminal; `TCGETS` stayed `-ENOTTY` until the termios layer arrived), then `readlink("/proc/self/fd/0")` yields `/dev/console`, and `stat("/dev/console")` is compared with `fstat(0)` on `(st_dev, st_ino)`. Both paths report the same console identity, so they match and `ttyname(0)` returns `/dev/console`.

## Compared to Linux

Linux's `devtmpfs` and `procfs` surface device nodes and process state as a mounted synthetic namespace, with `->poll` and `->read` driver hooks and per-task `/proc` files. Rach2's version is the same shape, a path classifier in front of driver-backed inodes, minus the VFS mount plumbing.

## Deliberate simplifications

* A bare `ls /` does not yet merge the `dev` and `proc` mount names into the KRFFS root listing; they are reached by their absolute paths (`opendir("/proc")` works).
* `openat(dirfd, rel)` relative to a synthetic directory fd is not resolved; busybox uses absolute `/proc/<pid>/...` paths.
* `comm` and `cmdline` carry the program basename, not the full argv; `stat`'s sizes and cpu-time fields are zero.

## Verify

* `tests/devprobe.c` (freestanding): `/dev/zero` reads zeros, `/dev/null` is EOF and a sink, `/dev/full` is `-ENOSPC`, `/dev/urandom` varies (the probe runs with trusted entropy); `/proc/self/stat` reports the caller's pid and comm; `/proc/self/fd/0` resolves to `/dev/console`. Prints `DEVPROBE OK`.
* `tests/bootprobe.c` (freestanding, `make bootpart-test`): `/proc/cmdline` at each layer (content, the `/proc` listing, stat/fstat identity, the fd's readlink origin) and the two raw windows. Three boots of one disk. Without a FAT partition, `/dev/bootpart` must be absent from stat and open alike (`BOOTPROBE ABSENT OK`). With one of known bytes, the device is driven end to end (`BOOTPROBE OK`). And a third boot mounts a RAM image instead of the disk and requires both `/dev/bootpart` and `/dev/grubcore` anyway, drives the gap window's extent and every refusal at its edges, and proves a descriptor opened as root stops accepting writes once the process drops privilege (`BOOTPROBE RAMROOT OK`).
* `tests/host/synthfs_hosttest.c`: the path classifier under ASan and UBSan.
* `tests/host/mbr_hosttest.c`: the MBR parse and the boot-partition judgement over crafted tables.
* `tests/musl/devtest.c` (installed `mdevtest`): the devices, `ttyname(0)`, and `opendir` over `/dev` and `/proc`, all through real musl.
