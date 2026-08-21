# Rach2 File System (KRFFS v2)

A small hierarchical file system with real directories and the modern `*at` file API a libc builds `FILE*` and `DIR*` on. KRFFS has directories and path resolution, so musl's `fopen`, `stat`, `opendir`, and `mkdir` work and a navigable tree exists: a program can `mkdir` a directory, create and read a file inside it, `chdir` into it and resolve relative names, list it, and `cat /etc/motd` from a tree the host staged into the image.

## Pieces

| File      | Responsibility                                                                                                                                  |
|-----------|-------------------------------------------------------------------------------------------------------------------------------------------------|
| `krffs.*` | The on-disk format, the linear node list, and the v2 path resolver (`krffs_walk`/`krffs_resolve`, `krffs_node_path`, `mkdir`/`rmdir`/`readdir`) |
| `fd.*`    | The per-process descriptor table: type, the resolved node or socket, offset, and flags (READ/WRITE/APPEND/CLOEXEC)                              |
| `procfs.*`| The synthetic `/proc`: each file snapshotted into a bounded buffer at open and served from there                                                |

The descriptor, path, directory, and `*at` syscalls live in `../syscall.c`; this directory is the file system itself plus the descriptor table it attaches to.

## On-disk format

KRFFS is a flat linear list of nodes packed into one image: each `struct krffs_node` header (magic, type, id, `name[256]`, mode, uid, gid, three timestamps, `size`, `previous_node_size`, `data_size`, `parent_id`) is followed by its `data_size` bytes, and the next node sits `size` bytes on. The header is byte-identical in the kernel (`krffs.h`) and the host tools (`krffs/krffs_node.h`), so an image written by the host mounts in the guest.

v2 added one field, `parent_id`, at the end of the header (so `sizeof` propagates to every host tool automatically). That single field turns the flat list into a tree:

* A directory is a node whose `mode & S_IFMT == S_IFDIR` (`0040000`); a regular file has bare permission bits (no type bit), which keeps old flat images readable.
* A directory has an id `mix(parent_id, name)`, an FNV-style mix of the parent's id and the name, never zero. A regular file keeps the older hash of its own name alone, which is enough because what the id is used for is naming a parent. Siblings have unique names, so the id is unambiguous and survives a move (it depends on name and parent, not offset).
* The root has the reserved id `KRFFS_ROOT_ID == 0`. Host-created nodes leave `parent_id` zero, so they resolve as root's children with no host change; the root node itself is matched by pointer (`node == fs->node`) since `krffs_find_node_by_id` deliberately never hands back id 0.

## On a real disk

KRFFS mounts three ways, chosen at boot: the RAM-disk multiboot module in place (the default), the same module through the page-cache window (`rach2.fswindow`), or a real disk partition through that window (`rach2.fsdisk`). The file system code is identical in all three; it only ever pointer-walks its node list, and on the window paths that walk is demand-paged.

On the disk path the kernel reads the first sector of every non-empty partition entry and takes the first that holds a real KRFFS root, judging the root itself, not the MBR type byte, so a GRUB-partitioned disk with its boot partition first still mounts and maps it into a fixed kernel-virtual window (`PCWIN_BASE == 0xD0000000`, above the region where real firmware places device MMIO, spanning up to the lowest device BAR that lands in it and capped at `PC_WINDOW_CAP == 512 MiB`, with the partition clamped to fit) backed by a fixed-size page cache (`../mm/pagecache.c`). A fault in the window polls the missing page in from the disk through the polled block driver behind `block0` (`../block/ata.c`, or `../block/ahci.c` when `rach2.disk=ahci` selects it); a write dirties it; eviction and `fsync`/`sync` write dirty pages back with a FLUSH CACHE. So on the disk path `fsync` produces real durability: a written-and-synced file survives a reboot, proved by the two-boot `tests/persistprobe`. On the RAM-module path the image is already as durable as memory, so `fsync` is a no-op. A disk KRFFS must fit the window and KRFFS's 32-bit size, so the host tools cap a partition at 512 MiB and any KRFFS at just under 4 GiB.

## Path resolution

`krffs_walk` is the resolver. It splits a path on `/` and, starting at the root (or a given directory id), matches each component by `name` and `parent_id`, descending only into directories. `.` keeps the current directory; `..` follows the directory's own `parent_id` (and resolves a root parent back to the root, not to NULL). `krffs_resolve_e` reports whether a failure was a missing component (`-ENOENT`) or a non-directory in the middle of the path (`-ENOTDIR`), which the syscall layer returns verbatim. `krffs_resolve_x` adds the third: a `may_search` predicate the resolver asks about every directory it enters, the root and the `.` and `..` arms included, whose refusal (`-EACCES`) becomes the walk's error. The file system knows nothing about credentials, so the rule lives in the predicate the syscall layer installs (`cred_may_search`, by the effective ids; `cred_may_search_real` for `access(2)`, which answers for the real user and must charge the prefix the same way it charges the target). The predicate reads the calling process, so it is only valid from syscall context; the boot self-test, the debug shell, and the re-resolves that confirm a descriptor still names its node pass NULL and are unchanged. The target itself is never charged: reaching a name is not reading it.

`-EACCES` from a denied ancestor outranks what the caller would otherwise learn. A missing name inside a directory the caller may not search reports `-EACCES` and not `-ENOENT`, so existence cannot be probed through it, and `open` answers it before considering `O_CREAT`, whose fall-through creates through a resolver that does not charge search. The mutating calls below it have no such reporting: `krffs_unlink` and `krffs_rename` answer every refusal with a bare `-1`, which userspace decodes as `EPERM` whatever went wrong, so `sys_unlink` and `sys_rename` decide each case themselves before calling down and the call that follows can only succeed. Unlink gives `-ENOENT` for a missing path and `-EISDIR` for the root or any directory. Rename gives `-ENOENT` for a missing source or a missing destination parent, `-ENOTDIR` when that parent is not a directory, `-EINVAL` for a destination naming no entry, `-EPERM` for a directory source (its children reference its id, so a move would orphan them: POSIX reserves `EPERM` for a rename the file system cannot perform), and `-EISDIR` for a directory destination under a plain source, which is the case `EISDIR` is actually reserved for. The root is refused by node identity, not by `KRFFS_IS_DIR`, because the root has `0755` with no type bits and would otherwise read as a plain file. `krffs_node_path` walks `parent_id` upward to rebuild a node's absolute, canonical path (used by `getcwd`, `chdir`, and dirfd resolution; `..` and `.` collapse for free because it follows real links).

`open`, `stat`, `unlink`, `rename`, `mkdir`, `rmdir`, `getdents64`, and `chdir` all route through the walk, so `/etc`, `/bin`, and a `mkdir`'d subtree are real directories, not a flat namespace.

## The current directory and the `*at` API

Each process has an absolute, canonical `cwd` string in its PCB (`"/"` at start, inherited across `fork`). A relative user path is joined onto the cwd at copy-in (`copy_path_cwd`), so the existing root-relative resolver, which already honors `.` and `..`, walks the right tree; an absolute path passes through. `chdir` resolves the target, requires a directory, and stores its canonical path; `getcwd` (183) returns it.

The `*at` calls let a libc build `FILE*` and `DIR*` and resolve against a directory descriptor:

* `openat` (295) and `fstatat64` (300): resolve relative to `AT_FDCWD` (the cwd), an absolute path, or a real directory fd (`copy_path_at` rebuilds the dir's path with `krffs_node_path`).
* `fstat64` (197): `stat` by descriptor; the root, a device, and a socket report `S_IFDIR`, `S_IFCHR`, and `S_IFSOCK`.
* `_llseek` (140): 64-bit seek; KRFFS offsets are 32-bit, so a seek past 4 GB is rejected, not truncated.
* `fcntl64` (221): `F_DUPFD(_CLOEXEC)`, `F_GETFD`/`F_SETFD` (the close-on-exec bit), `F_GETFL`/`F_SETFL` (access mode and `O_APPEND`).
* `O_CLOEXEC` and `O_APPEND` are honored at `open`; `execve` drops `FD_CLOEXEC` descriptors at its commit point.

## Timestamps

Every node has three: access, modification and attribute-change. They are
reported by every path that fills a stat, and there are more of those than there
look: `krffs_fill_stat64` for a path, `fstat_fill` for a descriptor,
`do_old_stat` for the narrower `stat(2)` ABI, `synth_stat` for `/dev` and
`/proc`, and a root branch of its own in each of `stat64`, `fstatat64` and the
old ABI. Every one of them zeroed all three until milestone 21, so every file on
this system reported 1970 and nothing could tell a source from the object built
from it.

The root directory reports its own node's times. A synthetic node reports the
BOOT time: it has no history, zero would be a claim about 1970, and now would be
worse, since a device whose modification time advanced on every stat would look
freshly written to anything comparing times.

The kernel stamps them, and stamping is the file system's own job rather than
each caller's: `krffs_stamp` moves the modification and attribute-change times
together when a file's CONTENTS change (a write, a truncate, the moment a node
is created) and the attribute-change time alone when only its attributes do (a
chmod, a chown that changed an owner). A directory is a file whose contents are
its entries, so creating, removing, renaming or replacing a name stamps the
DIRECTORY it happened in, both directories when a rename crosses two, and the
root like any other. `rename(path, path)` and `chown(path, -1, -1)` change
nothing and stamp nothing, which POSIX requires and which also matters here: a
call any caller may make must not be a way to touch a file.

Removing a name whose file somebody still holds open goes through
`krffs_unlink_held`, which stamps and orphans in one place, because two callers
reach it (the `unlink` syscall and the rescue shell's own `rm`) and a stamp
written at each of them is a stamp one of them will lack.

Access time is stored and reported but NEVER advanced by a read. That is a
`noatime` mount, and the reason is this file system's own: it lives behind a
write-back page cache, so stamping an atime on every read would dirty a page on
every read and turn a reading workload into a writing one. An atime set on
purpose through `utimensat` is kept and reported, which is why this is not the
same thing as reporting the modification time in its place.

## Deliberate simplifications

What KRFFS deliberately leaves out, without compromising the tree structure:

* The permission model stops short of a few classic pieces. The mode is the twelve low bits: nine permission bits, enforced by one access check at every path that opens, creates, execs, or walks, plus the setuid, setgid, and sticky bits stored in the 07777 width. Exec honors setuid and setgid (the effective and saved ids take the file owner's, the real ids unchanged, and `AT_SECURE` reports the change to libc); this is how an unprivileged process regains privilege, the mechanism behind `su`. A modification takes the set-id bits away again. Setuid always goes; setgid goes only when the group-execute bit is set beside it, which is the same condition `execve` grants on, so a file that cannot hand out a gid does not have one taken away. The rule runs after a write that moved bytes, after a truncate (`O_TRUNC` and `creat` alike), and after a chown that actually changed a field. Write and truncate spare a privileged caller, or root would disarm `/bin/su` by rewriting it; chown does not, which is why `cp` and `install` chown before they chmod. The changed-field condition on chown is a safety requirement, not an optimization: `chown(path, -1, -1)` needs no right, because nothing moves, so a clear that ran on it would let any caller disarm any set-id file with one call. `chmod` has its own rule, since an owner may mint a set-id mode with no write at all: an unprivileged caller storing a mode on a file whose group is not one of theirs does not keep setgid.

The root node is excluded from all of it by node identity and not by `KRFFS_IS_DIR`, because `mkfs` stamps the root a bare `0755` with no type bits and `KRFFS_MODE_DIRTY` is the same bit as `KRFFS_MODE_SUID`: a clear that reached the root would report an unclean image as cleanly unmounted, which is corruption, not a permission bug.

The sticky bit is enforced on a directory, which is what makes a shared one usable: with it set, write permission is no longer enough to take an entry out, and only the entry's owner, the directory's owner, or a privileged caller may unlink it, rmdir it, rename it away, or rename over it. That last one is not an extra: replacing an entry removes it, so a rule that stopped at unlink would leave the whole attack open. The answer is `-EPERM`, which is the errno POSIX names here, and it is asked after the directory's write check so a caller with no write permission still learns that first. The SVR4 relaxation that also permits removal when the caller can write the file is not POSIX and is not adopted: it would let a victim's own choice of mode decide who may delete their file. `1777 /tmp` in the os-real image is the one sticky directory the tree ships, and the SSH capstone exercises it.

The setuid bit shares the position of the root node's dirty-mount flag (`KRFFS_MODE_DIRTY`), which is safe because that flag lives only on the root, a node every permission answer and every set-id clear excludes by identity, not by reading its mode.

What stays out, each with its cost, not as a list. Quotas and ACLs need room the node header does not have: both would be a v3 on-disk format, coordinated across the kernel and two host tools, to store a per-user table or a per-file entry list. Capabilities are not absent so much as unfactored: the `euid == 0` tests scattered through the syscall layer are this system's capability model, and naming them (`CAP_FSETID` at the set-id clear, `CAP_FOWNER` at chown) is the exercise that would turn them into one. Per-user resource limits are not deferred at all: `getrlimit` and `prlimit64` report the kernel's real fixed pools and `setrlimit` is a documented no-op, which is an accurate report, not a missing feature. The access check judged the target and its immediate parent alone until the resolver started charging search on every directory it enters.
* Linear scans: directory lookup and `getdents` walk the node list (no hashing or B-trees), which is fine at teaching scale.
* A directory cursor is the byte offset of the last node returned, not a count of the children seen. A count is what this had, and it is wrong for the one pattern that matters: `rm -rf` reads a batch of names, unlinks them, and reads on from the cursor, so every unlink renumbered the survivors and the next read skipped one entry per file already gone. The leftovers made `rmdir` refuse and the user saw only "Directory not empty". An offset survives deletion, because removing a node only widens free extents and allocation splits a free node instead of shifting a reserved one. It does not survive a file GROWING out of its extent, which relocates that node; a child that grows while another descriptor is mid-listing can be missed or repeated. That is a concurrent modification of a directory being read, which POSIX leaves unspecified, and it is not reachable from the loop the cursor exists for. `tests/unlinkdirprobe.c` has the reachable case.
* No hard links, no symlink chains, no journaling. `AT_SYMLINK_NOFOLLOW` is accepted but inert.
* Open-file descriptions are refcounted (the shared seek offset). `fd_entry_t` holds a `struct open_file *`, a small refcounted cell from a fixed pool (`fs/fd.c`) carrying the seek offset, so `dup`/`dup2`/`fork`/`F_DUPFD` share one offset the Unix way (a read or seek through one descriptor is seen by the others), and closing one descriptor leaves the duplicates working until the last reference goes; `release_fd_table` drops every reference when a process dies, so the pool never leaks. The backing object (KRFFS node or socket) and `FD_CLOEXEC` stay per-descriptor (a socket is refcounted per fd; `dup`/`dup2` clear the new fd's CLOEXEC, `execve` closes the marked ones). Probe: `tests/ofdprobe.c` (`OFDPROBE OK`) and the musl `tests/musl/ofdmusl.c` (`OFDMUSL OK`). A descriptor open on a regular file names that file from open to close: across an `unlink`, across a `rename` onto its name, and across a write that relocates it. `unlink` and `rename` ask the descriptor tables whether anyone holds the node (a scan of every process's table, deliberately not a counter on the node, since a counter would have to be maintained in `fork`'s table copy, in `dup`, in `dup2`, and in every future path that copies a descriptor, and one missed increment is a use-after-free no test would show). A held node is orphaned, not freed: its name is cleared, which makes it unmatchable, because the resolver never hands the matcher a component shorter than one character. The last descriptor to close frees it, in `fd_release_backing`, where every other backing kind is already released. Being unmatchable covers the resolver alone, so the three scans that enumerate by `parent_id` skip a nameless node too, and an orphan is invisible to `getdents64`, to the debug shell's lister, and to `rmdir`'s emptiness test, which is what lets `rmdir` succeed over a directory holding only orphans the way POSIX says. There are two offsets in that cell, not one. Everything this kernel opens as a file is under 4 GiB, so `offset` is 32 bits and every path that reads or writes one stays in 32-bit arithmetic; the three raw disk windows (`/dev/bootpart`, `/dev/grubcore`, `/dev/disk0`) are not, because a whole drive needs a 64-bit offset to address its far end, and they use `raw_offset` instead. Widening the field every other subsystem shares would have put the whole file, procfs and terminal layer in reach of a change only the disk windows needed. Both live in the open-file description, so both are shared across `dup` and `fork` alike; `make bootpart-test` proves the wide one is by duplicating a descriptor parked at 258 GiB and reading through the copy.

That closes the window this file used to describe, where the cached-node round-trip could not tell a freed node from a different file the allocator later placed at the same address, and a stale descriptor would then act on the new file. It was not theoretical: a reader holding a file across a `rename` onto its name read the *replacement's* bytes, silently, with no error. No address a descriptor holds is handed out while it holds it, so the round-trip is now a resolution, not an identity test: an orphan is live, and a named node must still resolve back to itself. The regression is `tests/fdstale.c` (`FDSTALE OK`), whose assertions this reversed: it used to require `-EBADF` from a read through an unlinked-under descriptor, which is the inverse of what POSIX asks.

Every path that answers "what files are here" hides an orphan: `getdents64`, the debug shell's `ls` and its child lister, the file count in the boot banner, `rmdir`'s emptiness test, and the host tools' lookup and FUSE readdir. The tools that operate on the medium, not on the namespace still see them, and must: `fsviz` draws what is physically there, `defrag` relocates every reserved node whether or not a name reaches it, and `fsck.krffs` reports one before reclaiming it.

The debug shell's `rm` and `mv` follow the same rule instead of being exempted. They call the file system directly, below the syscall layer that owns the holder decision, and the rescue shell can resume while a transient init sits blocked holding a file; freeing that node would hand its address back to the allocator with a descriptor still on it, which is the aliasing this contract exists to prevent. The host tools honour it too: their flat by-name lookup matches a stored DJB id, which an orphan keeps, so it tests the name as well and a cleared name is unreachable there for the same reason it is here.

A shutdown that does not close descriptors strands an orphan on a persistent image, with nothing left to free it. The mount reaps every nameless reserved node before anything opens a descriptor, which is the one moment the tables are known to be empty, and `fsck.krffs` reclaims them offline too. The reap is not gated on the dirty flag: a clean halt closes no descriptors either, so a cleanly unmounted image can carry one, and gating would have stranded the node permanently the moment `fsck.krffs` cleared the flag.

Directories stay out, as a scope line and not an oversight: `rmdir` still frees the node under an open directory descriptor, and a relative path through one still answers `-EBADF`. A directory descriptor here is a name, which is what the per-component search check rests on, and an orphan directory has no name for a relative path to join.
* The interactive host FUSE mount stays flat. Images gain directories through `krffs/stageimage.krffs` (the manifest-driven builder, which writes v2 directory nodes directly) and the in-guest `mkdir`; teaching the 1100-line FUSE mount to browse directories is a later convenience, not needed by the kernel or the integration.

## The /proc snapshot, and its bounds

A `/proc` file is generated into a buffer when it is OPENED and read back from there, so one descriptor shows one moment. Regenerating on each read is the other way to build it, and it is the wrong one here: a table that changed between two reads would be spliced together across the boundary, and every pool these files describe has a size the kernel chose, so a worst case can be argued instead of measured.

That is what the bounds are. Each file declares its own maximum (`PROCFS_MAX_*`), the buffer is the largest of them, written out rather than derived, and a `typedef` of a negative array size stops the build if a declared bound will not fit, since C89 has no static assert. The appenders no longer stop when they run out of room: they record it, and a text that exceeded its bound is refused with `-EOVERFLOW` and one line on the serial log naming the file. A sixteen-row table read back as a nine-row one with nothing anywhere saying so is the failure this replaced.

`tests/procprobe` proves both halves: it opens `/proc/<pid>/stat` for a child, kills and reaps the child, and requires the still-open descriptor to answer what it took at open, which a generator asked at that moment could not; and it requires each file it can open to come back ending where its last line ends, which a bound set too small turns into a refusal rather than a short read.

## The machine's own /proc files

Beside the per-process tree, `/proc` carries four files about the machine: `uptime`, `stat`, `meminfo` and `mounts`. Every number in them is one the kernel already holds, and where Linux prints a field this system has no equivalent for the field is zero rather than an invention. `top` is the applet that needs the fields that are not here, so it is left not working and said to be, in place of one printing plausible constants.

Idle time is the one number that had to be built. There is no idle task: `schedule()` returns with the blocked process still current when nothing is ready, and the caller parks the processor with `sti; hlt` inside its own syscall, so a tick cannot be classified by asking which process is current. A marker is set around the halt and sampled by the timer at entry, before any wakeup can change a process's state, and the two counters record ticks the timer DELIVERED rather than wall time, because interrupts are off in several paths here and a tick can be lost. Every halt in the kernel goes through `idle_wait_irq_off` or `idle_wait_irq_on` in `../proc/sched.c`, and `make verify-style` refuses a bare `sti; hlt` written anywhere else: a site added later and left unmarked would report its idle time as busy and nothing would say so. The bare `hlt` the debug shell's audio player uses is deliberately outside that rule and counts as busy: it runs with the scheduler stopped, where nothing is going to read `/proc/uptime` anyway, and its interrupt flag is already whatever the caller left.

`btime` needs a boot epoch that does not move, and `rtc_boot_time` is the moment the clock was READ, some way into the boot. `rtc_boot_epoch` is that reading less the uptime it was taken at, so it names uptime zero and answers the same on every read within one boot.

`/proc/mounts` has a source, because there is not one to derive afterwards. This file system can be mounted from a real disk, from the RAM module through the page-cache window, or from the module in place, and what survives those three branches is one bit, disk or not. So the mount records what it mounted and with which options at the moment it succeeds, and the file reports that; a fixed line would call a RAM root a disk one on two boots out of three. `make procfiles-test` boots all three and requires each to name its own.

`df` needs `statfs64` (268) and `fstatfs64` (269), which returned `-ENOSYS` before. KRFFS knows its own size and the usable bytes of its free nodes, so both numbers are real; the block size is 1024, chosen because that is the unit `df` prints in by default, and `f_files` is left zero, which says "not counted" rather than claiming an inode budget this format does not keep.

## The network files under /proc

`/proc/net` carries `dev`, `route`, `arp` and `tcp`. Each format was taken from the program that reads it rather than from a description: `route` reads eleven whitespace-separated fields and skips one header line, `arp` reads six and skips one, and `netstat` reads nine and rejects a line that yields fewer. A file that is nearly right fails in the reader and not here, so the columns Linux prints past the ninth are present rather than trimmed, for fidelity rather than because a parser here needs them. This stack's TCP states are mapped to Linux's numbers rather than cast: a reader prints the name that goes with the number it is given, so an unmapped value names the wrong state instead of a missing one.

Each table's declared bound is checked against the table behind it at compile time, so growing the connection pool or the ARP cache fails the build rather than truncating a file at run time.

The route table comes out of `netconf`, which knows one subnet and at most one gateway. The ARP cache and the tcb pool are private and written from receive context, so each gets a lock-protected snapshot function rather than a reader walking the table an interrupt is changing.

Identity is the part worth explaining. `netstat` parses a uid and an inode, and a socket had neither; printing zero would have said every socket on the machine belongs to root and is the same socket. Each socket now carries an inode of its own, monotonic within a boot so a reused slot never reads as the earlier connection, and a uid taken in syscall context where there is a caller to ask. A passive connection is allocated in receive context, where there is not, and inherits the LISTENER's uid, which is whose socket it is. Both values are copied into the control block as well, because a row stays in the pool through the closing states while `tcb->sock` is cleared the moment the descriptor goes: read through the socket, a connection in `TIME_WAIT` would report owner 0 and socket 0. The same inode appears in `/proc/net/tcp`, in `/proc/<pid>/fd/N` and in `fstat`, so one socket is one thing to a program reading all three; it was the constant `socket:[1]` before.

`/proc/net/tcp6` stays absent. There is no IPv6 here, and `netstat` says so for that family alone after printing the IPv4 table, which is the intended answer: a file that existed and was empty would tell it the family is present with nothing on it.

## What a descriptor names

`/proc/<pid>/fd/N` is a symbolic link, and what it resolves to is meant to identify the object, not just its kind. A file names its path, a device its `/dev` name, a pty slave `/dev/pts/N`, a `/proc` snapshot the path it was opened by. A socket names `socket:[<inode>]` and a pipe `pipe:[<inode>]`, each the object's own number, monotonic within a boot so a reused pool slot never reads as the thing that had it before; `fstat` on the same descriptor reports that same number. Both were the constant 1 before, which told a reader that every socket on the machine was one socket and every pipe one pipe.

In a login over SSH all three standard descriptors name the session's pty slave, since the server hands the shell the same terminal three times; a session that asked for no terminal is on pipes instead, and names them. Listing that directory has one entry the listing program cannot resolve, its own descriptor on the directory, which it has closed by the time it prints: the kernel is right to say it is gone, and `ls` reports the miss rather than inventing a name.

## Testing

* `tests/dirprobe`: `mkdir`, nested create/write/read, duplicate-`mkdir` and create-in-missing-dir rejection, non-empty `rmdir` refusal, `getdents` listing exactly a subdir's children, the `*at` calls (`openat`, `fstat64`, `fstatat64`, `_llseek`, `fcntl64`), `O_APPEND`/`F_DUPFD`/`O_CREAT`-mode, `chdir` plus `getcwd` plus relative resolution plus a dirfd-relative `openat`, `ENOTDIR` versus `ENOENT`, and `/x/..` resolving to the root. Prints `DIRPROBE OK`.
* `tests/musl/catfile`: a musl program that reads a host-staged `/etc/motd`, `opendir`s `/etc`, then `mkdir`s a directory, writes and reads a file inside it, `stat`s it, lists it, and cleans up, all through `<stdio.h>` and `<dirent.h>`. Prints `CATFILE OK`.
* `tests/procprobe`: the `/proc` snapshot and its bounds; see the section above. Prints `PROCPROBE OK`.
* `make procfiles-test`: the machine's own files and its network files, on the boots that differ and through the programs that read them. Three boots of the test image, one per mount source, each of which must name its own; then `ps`, `df`, `free`, `uptime`, `dmesg`, `route -n`, `arp -n` and `netstat -tn` on the real image, checked against numbers the guest does not hold: the shell's own pid, the megabytes the image was built at, the address and gateway QEMU's user network hands out, and the peer address of a connection opened into the machine from outside while `netstat` runs. `dmesg` is the one that is not an applet: it is a script that prints `/dev/bootlog`, and what is checked is that the name reaches the log.
* `krffs/stageimage.krffs` stages an `/etc` tree (`/etc/motd`, `/etc/hostname`) into an image from its manifest at build time, proving the host tools speak the hierarchical format.
