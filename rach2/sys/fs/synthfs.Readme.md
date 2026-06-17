# Synthetic file systems: `/dev` and `/proc`

Not every path names a file on disk. `/dev/null` is a kernel sink; `/proc/self/fd/0` is a window onto a live process. These are synthetic file systems: the names are backed by in-kernel drivers and process state, generated on demand, never stored in KRFFS. Rach2 mounts two, a `/dev` (`devfs`) and a minimal `/proc` (`procfs`), the slice musl and busybox actually read.

## One classifier, many syscalls

The temptation is to scatter `if (path starts with "/dev")` checks in front of every `krffs_resolve`. That rots. Instead a single pure classifier, `synth_classify()` in [synthfs.c](synthfs.c), turns a path into a `vnode_kind`:

```
""                  -> VNODE_KRFFS        (the real root; resolve via KRFFS)
"dev"               -> VNODE_DEV_DIR
"dev/null"          -> VNODE_DEV          (name = "null")
"proc"              -> VNODE_PROC_DIR
"proc/self"         -> VNODE_PROC_PID     (pid = SELF)
"proc/7/stat"       -> VNODE_PROC_FILE
"proc/7/fd"         -> VNODE_PROC_FD_DIR
"proc/self/fd/0"    -> VNODE_PROC_FD_LINK (a magic symlink)
"hello.txt"         -> VNODE_KRFFS
```

Every path-taking syscall (`open`, `stat`, `fstatat`, `readlink`, `getdents`, `mkdir`/`unlink`/`rename`, and the rest) calls it once: a `VNODE_KRFFS` result means "ordinary file, hand it to KRFFS", anything else routes to the devfs and procfs glue. The classifier is pure string logic, so the fence-post-prone component parsing runs natively under ASan in `tests/host/synthfs_hosttest.c`.

## devfs: character devices over `file_ops`

A small registry maps a leaf name to a character device: a `file_ops` vtable plus a `{major, minor}`. `open("/dev/<name>")` binds that vtable onto the descriptor (`FD_TYPE_DEVICE`, `priv` is the registry entry), after which the generic read, write, and close paths dispatch through it, so the device code lives nowhere in the syscall layer. The set, with the real Linux device numbers:

| device             | major:minor | behavior                                                                 |
|--------------------|-------------|--------------------------------------------------------------------------|
| `null`             | 1:3         | read returns EOF, write is discarded                                     |
| `zero`             | 1:5         | read returns zeros, write is discarded                                   |
| `full`             | 1:7         | read returns zeros, write returns `-ENOSPC`                              |
| `random`/`urandom` | 1:8 / 1:9   | read returns the trusted CSPRNG, else `-EIO` (see the krandom Readme)    |
| `console`          | 5:1         | the kernel console (`FD_TYPE_CONSOLE`)                                   |
| `tty`              | 5:0         | the caller's controlling terminal: the console, a pty slave, or `-ENXIO` |
| `ptmx`             | 5:2         | a clone device: each open mints a new pty pair and returns the master    |

`lseek` on the mem devices returns 0 (Linux's `null_lseek`); the stream and terminal devices are `-ESPIPE` (a valid fd, just not seekable). `stat` and `fstat` report a character device (`S_IFCHR`) with a synthetic `st_dev` distinct from KRFFS's, so a device's `(st_dev, st_ino)` identity never aliases a real file's.

## procfs: process state as files

Generated per query from the process table:

* `/proc/<pid>/stat`: the space-separated fields ps parses (pid, `(comm)`, state, ppid, and the rest). Accounting fields the kernel does not track are zero, but the shape is exact and the `comm` parentheses are scrubbed so a naive parser is not fooled.
* `/proc/<pid>/cmdline`: the program name (the kernel keeps `comm`, not the full argv).
* `/proc/<pid>/fd/<n>`: a magic symlink whose `readlink` returns the descriptor's path (`/dev/console` for the console, `/dev/<name>` for a device, and so on).
* `/proc/self` is the caller; `getdents("/proc")` lists `self` and the live PIDs.

A `/proc` text file is snapshotted into a pooled buffer at open and served from there, so a read sees one consistent moment and dup and fork share the snapshot until the last descriptor closes (the open-file-description lifetime, freed by the `file_ops` close hook). Text is built with bounded append helpers and hand-rolled integer formatting: no `snprintf`, no 64-bit division (the kernel has no libgcc).

## What `ttyname(0)` exercises

`ttyname` is the integration target, and it pulls the whole thing together. musl's `ttyname_r` (verified against the vendored `musl-1.2.5`) issues `isatty(fd)` as `ioctl(TIOCGWINSZ)`, which the console answers (so it reads as a terminal; `TCGETS` stayed `-ENOTTY` until the termios layer arrived), then `readlink("/proc/self/fd/0")` yields `/dev/console`, and `stat("/dev/console")` is compared with `fstat(0)` on `(st_dev, st_ino)`. Both paths report the same console identity, so they match and `ttyname(0)` returns `/dev/console`.

## Compared to Linux

Linux's `devtmpfs` and `procfs` surface device nodes and process state as a mounted synthetic namespace, with `->poll` and `->read` driver hooks and per-task `/proc` files. Rach2's version is the same shape, a path classifier in front of driver-backed inodes, minus the VFS mount plumbing.

## Honest simplifications

* A bare `ls /` does not yet merge the `dev` and `proc` mount names into the KRFFS root listing; they are reached by their absolute paths (`opendir("/proc")` works).
* `openat(dirfd, rel)` relative to a synthetic directory fd is not resolved; busybox uses absolute `/proc/<pid>/...` paths.
* `comm` and `cmdline` carry the program basename, not the full argv; `stat`'s sizes and cpu-time fields are zero.

## Verify

* `tests/devprobe.c` (freestanding): `/dev/zero` reads zeros, `/dev/null` is EOF and a sink, `/dev/full` is `-ENOSPC`, `/dev/urandom` varies (the probe runs with trusted entropy); `/proc/self/stat` reports the caller's pid and comm; `/proc/self/fd/0` resolves to `/dev/console`. Prints `DEVPROBE OK`.
* `tests/host/synthfs_hosttest.c`: the path classifier under ASan and UBSan.
* `tests/musl/devtest.c` (installed `mdevtest`): the devices, `ttyname(0)`, and `opendir` over `/dev` and `/proc`, all through real musl.
