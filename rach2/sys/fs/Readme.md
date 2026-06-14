# Rach2 File System (KRFFS v2)

A small hierarchical file system with real directories and the modern `*at` file
API a libc builds `FILE*`/`DIR*` on. KRFFS gained directories and path resolution
in Phase 6 so musl's `fopen`/`stat`/`opendir`/`mkdir` work and a navigable tree
exists: a program can `mkdir` a directory, create and read a file inside it,
`chdir` into it and resolve relative names, list it, and `cat /etc/motd` from a
tree the host staged into the image.

## Pieces

| File        | Responsibility                                                                 |
|-------------|--------------------------------------------------------------------------------|
| `krffs.*`   | The on-disk format, the linear node list, and the v2 path resolver (`krffs_walk`/`krffs_resolve`, `krffs_node_path`, `mkdir`/`rmdir`/`readdir`) |
| `fd.*`      | The per-process descriptor table: type, the resolved node or socket, offset, and flags (READ/WRITE/APPEND/CLOEXEC) |

The descriptor → path/dir/`*at` syscalls live in `../syscall.c`; this directory is
the file system itself plus the descriptor table it attaches to.

## On-disk format

KRFFS is a **flat linear list of nodes** packed into one image: each
`struct krffs_node` header (magic, type, **id**, `name[256]`, mode, uid, gid,
three timestamps, `size`, `previous_node_size`, `data_size`, **`parent_id`**) is
followed by its `data_size` bytes, and the next node sits `size` bytes on. The
header is **byte-identical** in the kernel (`krffs.h`) and the host tools
(`krffs/krffs_node.h`), so an image written by the host mounts in the guest.

v2 added one field, `parent_id`, at the end of the header (so `sizeof` propagates
to every host tool automatically). That single field turns the flat list into a
**tree**:

* A **directory** is a node whose `mode & S_IFMT == S_IFDIR` (`0040000`); a
  regular file carries bare permission bits (no type bit), which keeps old flat
  images readable.
* Every node has a **unique id** `mix(parent_id, name)` — an FNV-style mix of the
  parent's id and the name, never zero. Siblings have unique names, so the id is
  unambiguous and survives a move (it depends on name + parent, not offset).
* The **root** has the reserved id `KRFFS_ROOT_ID == 0`. Host-created nodes leave
  `parent_id` zero, so they resolve as root's children with no host change; the
  root node itself is matched by pointer (`node == fs->node`) since
  `krffs_find_node_by_id` deliberately never hands back id 0.

## Path resolution

`krffs_walk` is the resolver. It splits a path on `/` and, starting at the root
(or a given directory id), matches each component by `name` **and** `parent_id`,
descending only into directories. `.` keeps the current directory; `..` follows
the directory's own `parent_id` (and resolves a root parent back to the root
rather than to NULL). `krffs_resolve_e` reports whether a failure was a missing
component (`-ENOENT`) or a non-directory in the middle of the path (`-ENOTDIR`),
which the syscall layer returns verbatim. `krffs_node_path` walks `parent_id`
upward to rebuild a node's absolute, canonical path (used by `getcwd`, `chdir`,
and dirfd resolution; `..`/`.` collapse for free because it follows real links).

`open`, `stat`, `unlink`, `rename`, `mkdir`, `rmdir`, `getdents64`, and `chdir`
all route through the walk, so `/etc`, `/bin`, and a `mkdir`'d subtree are real
directories rather than a flat namespace.

## The current directory and the `*at` API

Each process carries an absolute, canonical `cwd` string in its PCB (`"/"` at
start, inherited across `fork`). A **relative** user path is joined onto the cwd
at copy-in (`copy_path_cwd`), so the existing root-relative resolver — which
already honors `.`/`..` — walks the right tree; an absolute path passes through.
`chdir` resolves the target, requires a directory, and stores its canonical path;
`getcwd` (183) returns it.

The `*at` calls let a libc build `FILE*`/`DIR*` and resolve against a directory
descriptor:

* `openat` (295) / `fstatat64` (300) — resolve relative to `AT_FDCWD` (the cwd),
  an absolute path, or a real directory fd (`copy_path_at` rebuilds the dir's path
  with `krffs_node_path`).
* `fstat64` (197) — `stat` by descriptor; the root, a device, and a socket report
  `S_IFDIR`/`S_IFCHR`/`S_IFSOCK`.
* `_llseek` (140) — 64-bit seek; KRFFS offsets are 32-bit, so a seek past 4 GB is
  rejected rather than truncated.
* `fcntl64` (221) — `F_DUPFD(_CLOEXEC)`, `F_GETFD`/`F_SETFD` (the close-on-exec
  bit), `F_GETFL`/`F_SETFL` (access mode + `O_APPEND`).
* `O_CLOEXEC`/`O_APPEND` are honored at `open`; `execve` drops `FD_CLOEXEC`
  descriptors at its commit point.

## Deliberate simplifications

The honest hobby-OS line, drawn at the *right* place (not at the tree structure):

* **No users/permissions/ownership** — modes are stored but not enforced;
  `chmod`/`chown` are effectively no-ops.
* **Linear scans** — directory lookup and `getdents` walk the node list (no
  hashing or B-trees); fine at teaching scale.
* **No hard links, no symlink chains, no journaling.** `AT_SYMLINK_NOFOLLOW` is
  accepted but inert.
* **Descriptors are not yet refcounted open-file descriptions.** `fd_entry_t` is
  the open object, so `dup`/`fork`/`F_DUPFD` give the copy its own offset rather
  than a shared one, and a directory fd is not pinned against `rmdir` (a stale
  dirfd is rejected defensively by a round-trip check). The two-level
  descriptor → refcounted open-file-description split is **Part II** work, where
  pipes and ptys need the shared offset. `FD_CLOEXEC` is already per-descriptor.
* **The interactive host FUSE mount stays flat.** Images gain directories through
  `krffs/stagetree.krffs` (which writes v2 directory nodes directly) and the
  in-guest `mkdir`; teaching the 1100-line FUSE mount to browse directories is a
  later convenience, not needed by the kernel or the integration.

## Testing

* `tests/dirprobe` — `mkdir`/nested create/write/read, duplicate-`mkdir` and
  create-in-missing-dir rejection, non-empty `rmdir` refusal, `getdents` lists
  exactly a subdir's children, the `*at` calls (`openat`/`fstat64`/`fstatat64`/
  `_llseek`/`fcntl64`), `O_APPEND`/`F_DUPFD`/`O_CREAT`-mode, `chdir` + `getcwd` +
  relative resolution + a dirfd-relative `openat`, `ENOTDIR` vs `ENOENT`, and
  `/x/..` resolving to the root. Prints `DIRPROBE OK`.
* `tests/musl/catfile` — the integration milestone: a musl program that reads a
  host-staged `/etc/motd`, `opendir`s `/etc`, then `mkdir`s a directory, writes
  and reads a file inside it, `stat`s it, lists it, and cleans up — all through
  `<stdio.h>`/`<dirent.h>`. Prints `CATFILE OK`.
* `krffs/stagetree.krffs` stages `/etc/motd` and `/etc/hostname` into the test
  image at build time, proving the host tools speak the hierarchical format.
