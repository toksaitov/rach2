# Synthetic filesystems — `/dev` and `/proc`

Not every path names a file on disk. `/dev/null` is a kernel sink; `/proc/self/fd/0`
is a window onto a live process. These are **synthetic filesystems**: the names are
backed by in-kernel drivers and process state, generated on demand, never stored in
KRFFS. Rach2 mounts two — a `/dev` (`devfs`) and a minimal `/proc` (`procfs`) — the
slice musl and busybox actually read.

## One classifier, many syscalls

The temptation is to scatter `if (path starts with "/dev")` checks in front of every
`krffs_resolve`. That rots. Instead a single pure classifier, `synth_classify()` in
[synthfs.c](synthfs.c), turns a path into a `vnode_kind`:

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

Every path-taking syscall (`open`, `stat`, `fstatat`, `readlink`, `getdents`,
`mkdir`/`unlink`/`rename`, ...) calls it once: a `VNODE_KRFFS` result means "ordinary
file — hand it to KRFFS", anything else routes to the devfs/procfs glue. The
classifier is pure string logic, so the fence-post-prone component parsing runs
natively under ASan in `tests/host/synthfs_hosttest.c`.

## devfs — character devices over `file_ops`

A small registry maps a leaf name to a character device: a `file_ops` vtable plus a
`{major, minor}`. `open("/dev/<name>")` binds that vtable onto the descriptor
(`FD_TYPE_DEVICE`, `priv` = the registry entry), after which the generic
read/write/close paths dispatch through it — the device code lives nowhere in the
syscall layer. The set, with the real Linux device numbers:

| device | major:minor | behaviour |
|--------|-------------|-----------|
| `null` | 1:3 | read → EOF, write → discarded |
| `zero` | 1:5 | read → zeros, write → discarded |
| `full` | 1:7 | read → zeros, write → `-ENOSPC` |
| `random`/`urandom` | 1:8 / 1:9 | read → the ChaCha20 CSPRNG (`krandom_bytes`) |
| `console`/`tty` | 5:1 / 5:0 | the kernel console (open as `FD_TYPE_CONSOLE`) |

`lseek` on the mem devices returns 0 (Linux's `null_lseek`); the stream and terminal
devices are `-ESPIPE` (a valid fd, just not seekable). `stat` and `fstat` report a
character device (`S_IFCHR`) with a synthetic `st_dev` distinct from KRFFS's, so a
device's `(st_dev, st_ino)` identity never aliases a real file's.

## procfs — process state as files

Generated per query from the process table:

- `/proc/<pid>/stat` — the space-separated fields ps parses (pid, `(comm)`, state,
  ppid, …); accounting fields we don't track are zero, but the shape is exact and the
  `comm` parentheses are scrubbed so a naive parser isn't fooled.
- `/proc/<pid>/cmdline` — the program name (we keep `comm`, not the full argv).
- `/proc/<pid>/fd/<n>` — a magic symlink: `readlink` returns the descriptor's path
  (`/dev/console` for the console, `/dev/<name>` for a device, …).
- `/proc/self` → the caller; `getdents("/proc")` lists `self` and the live PIDs.

A `/proc` text file is **snapshotted into a pooled buffer at open** and served from
there, so a read sees one consistent moment and dup/fork share the snapshot until the
last descriptor closes (the open-file-description lifetime, freed by the `file_ops`
close hook). Text is built with bounded append helpers and hand-rolled integer
formatting — no `snprintf`, no 64-bit division (the kernel has no libgcc).

## What `ttyname(0)` exercises

`ttyname` is the integration target, and it pulls the whole thing together. musl's
`ttyname_r` (verified against the vendored `musl-1.2.5`): `isatty(fd)` issues
`ioctl(TIOCGWINSZ)` — which the console answers (so it reads as a terminal; `TCGETS`
stays `-ENOTTY` until the Phase-16 termios layer) — then `readlink("/proc/self/fd/0")`
yields `/dev/console`, and `stat("/dev/console")` is compared with `fstat(0)` on
`(st_dev, st_ino)`. Both paths report the same console identity, so they match and
`ttyname(0)` returns `/dev/console`.

## Real-OS analogue

Linux's `devtmpfs` and `procfs`: device nodes and process state surfaced as a mounted
synthetic namespace, with `->poll`/`->read` driver hooks and per-task `/proc` files.
Rach2's version is the same shape — a path classifier in front of driver-backed
inodes — minus the VFS mount plumbing.

## Honest simplifications

- A bare `ls /` does not yet merge the `dev` and `proc` mount names into the KRFFS
  root listing; they are reached by their absolute paths (`opendir("/proc")` works).
- `openat(dirfd, rel)` relative to a synthetic directory fd is not resolved — busybox
  uses absolute `/proc/<pid>/...` paths.
- `comm`/`cmdline` carry the program basename, not the full argv; `stat`'s sizes and
  cpu-time fields are zero. The full termios surface is Phase 16.

## Verify

- `tests/devprobe.c` (freestanding): `/dev/zero`→zeros, `/dev/null`→EOF/sink,
  `/dev/full`→`-ENOSPC`, `/dev/urandom` varies; `/proc/self/stat` reports our pid and
  comm; `/proc/self/fd/0`→`/dev/console`. Prints `DEVPROBE OK`.
- `tests/host/synthfs_hosttest.c`: the path classifier under ASan/UBSan.
- `tests/musl/devtest.c` (installed `mdevtest`): the devices, `ttyname(0)`, and
  `opendir` over `/dev` and `/proc`, all through real musl.
