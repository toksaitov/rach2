# Milestone 44: mounting KRFFS

## The goal

A KRFFS volume that is not the root can be mounted from inside Rach2: a partition of a disk or a stick, and an image file on any mounted file system, through a loop device. `mount(2)` accepts the type `krffs` as it accepts `vfat`, `exfat` and `ext2`, and what it places is read and written through the buffer cache like every other mounted type.

The milestone is complete when a KRFFS image the host tools built is mounted from a file on the machine's ext2 root, written, unmounted and found consistent by a strengthened `fsck.krffs`; when a KRFFS partition of the stick is mounted and written on the machine; when a FAT32 image file and an ext2 image file both mount through the same loop device, so the backing type does not matter; and when the format has one implementation in the kernel rather than two.

## What the tree settles before this plan

* KRFFS in the kernel is a memory file system. `rach2/sys/fs/krffs.c` walks a chain of nodes by pointer inside one image; `krffs_file_system_t` is a pointer and a 32-bit size (`krffs.h:115`). The root is that image: a Multiboot module, or a region of the boot disk mapped by the page-cache window, whose bound is 512 MiB (`pagecache.c:36`).
* The pointer reaches outside the file system. A descriptor holds a `krffs_node_t *` (`fd.h:127`), syscalls dereference it (`syscall.c:1318`, `1348`), and the dirty bit and the orphan scan work on the mapped root (`kernel.c:2079`, `syscall.c:124`). An offset core cannot be a local change to `krffs.c` alone.
* The KRFFS VFS type is not a mountable type's shape: its constructor ignores the device and the flags (`vfs.c:1140`), it has no `setattr`, `open`, `release`, `prepare_unmount` or `unmount` (`vfs.c:1122`), and its sync flushes the window (`vfs.c:905`). `vfs_krffs_type` is deliberately absent from `mount_types[]`.
* A mount's source is a partition window claimed as a physical sector range (`vfs.c:1556`, `devfs.c:1026`). A file has no such range, so a loop device must be its own claim identity and hold its backing file separately.
* A VFS node handle of NULL means the mount's root (`vfs.h:128`) and some syscalls require a non-NULL handle (`syscall.c:4706`, `4808`), so an offset handle cannot be the offset itself.
* The buffer cache's hold rules are checked and fatal: at most four buffers, and while any is held only a higher block on the same device (`bcache.c:56`). The same file also writes dirty victims during eviction (`bcache.c:167`) and during `bcache_sync` (`bcache.c:293`), and neither passes through that check.
* `bcache_write` promises the block has reached the device when it returns (`bcache.h:16`), which a write to a file on ext2 does not keep by itself (`ext2.c:918`).
* KRFFS operations take no mount lock today, because the root is memory and nothing sleeps (`vfs.c:632`, `926`). Every mountable type locks the mount around a mutation (`fat32.c:1509`).
* `fsck.krffs` walks by the forward size only (`fsck.krffs.c:379`), does not check the backlink the kernel enforces (`krffs.c:1641`), and clears the dirty bit before it validates the structure (`fsck.krffs.c:215`).
* BusyBox is built without `losetup` and without mount's loop support (`vendor/busybox/rach2.config:646`, `717`), and `/dev` and `/proc` name their entries in fixed tables (`devfs.c:919`, `synthfs.c:236`, `procfs.c:1297`).
* The machine of milestone 43: an ext2 root, a FAT32 scratch partition, a stick, a recovery path for a raw write, and the disk rule in `CLAUDE.md` for anything written to the notebook's disk.

## Decisions

### One implementation of the format, with the root on a facade

The node chain, the allocator and the validator stay in one file. Every operation takes a mount object and offsets, and reaches the image through a backend of `read`, `write`, `sync` and a size. The memory backend copies; the block backend goes through `bcache`. The root keeps a pointer facade over the memory backend until its callers outside the file system are migrated, so `fd`, the syscalls, the orphan scan and the dirty bit are not rewritten in this milestone.

### The handle is an offset, encoded

A node handle is its offset plus one, so the root's offset zero is not NULL. The encoding is stated once in the type's Readme and asserted in the host tests.

### The mountable type is a type like the others

A bounded pool of mount states, a namespace lock taken around every mutation, `prepare_unmount`, `unmount`, `setattr`, `statfs` and `sync`, the flags honoured, and the device taken from the source. `MS_RDONLY` mounts write nothing at all.

### The crash rule the format can keep

The forward chain is the invariant. A change is ordered so that every block the cache writes leaves a forward chain a mount can walk: data is written and synced before any field that publishes it, a split writes the new header into free space before the old node's size shrinks, and the backlink, which is derivable from the forward chain, is repaired by the check at the next mount rather than depended on. The dirty bit says a check is owed. A state the check cannot repair, a torn header among them, mounts read-only with the fault named, as a FAT32 volume does.

### The oracle is strengthened first

`fsck.krffs` gains the backlink check, validates before it clears the dirty bit, and reports what it repaired. Nothing in this milestone is proved by an oracle weaker than the kernel's own validator.

### The loop device

`/dev/loop0` to `/dev/loop3` over a regular file, attached and detached by a small Rach2 utility through a control node, and reported in `/proc/loop`; BusyBox's `losetup` stays out of the build. The file is held open for the attachment, its length fixes the sector count and is never changed by the device, a length that is not whole sectors is refused, a short read or write becomes a device error, and a file opened without write access attaches read-only, which the mount inherits. Holding it open is not enough on its own: while a file backs a device, a write or a truncation of it through any other descriptor is refused, because the image under a mounted file system may not change beneath it.

A loop transfer runs in the context of the process that asked for it and returns when the backing file has taken it: a write is a VFS write followed by a VFS `fsync` of the backing file, so the cache's promise that a written block has reached the device still holds. Detach unpublishes the node, refuses new mounts and opens, waits for the holds to go, syncs and invalidates every buffer of that device, and only then releases the file.

### The cache's ordering rule

Every block device names the device its backing lives on, or none. While a process holds buffers it may take a higher block on the same device, or any block on a device that is a strict ancestor of every device it holds. The same check applies where a dirty victim is written during eviction and where `bcache_sync` writes, which are the two paths that re-enter a device without asking for a buffer. A dependency cycle is refused at attach, and the depth a chain may reach is computed from the four-buffer limit and the holds each type takes, so the pool cannot be exhausted mid-chain.

### What is fixed here and what is not

Fixed: the offset core, the block backend, the mountable type, the loop device, the cache's rule, the utility, the oracle, and the records. Not fixed here: the disk root's window and the migration of the pointer facade's callers, hard and symbolic links, which the format has none of, and a volume larger than what a 32-bit image extent and 32-bit file offsets allow, which is a limit of this kernel and not of the format.

## Decisions on tests and tooling

* Host tests: the offset core over both backends against images `mkfs.krffs` built, with a model block device; the chain, the allocator's split and merge, the ends of the image, the handle encoding, and every refusal. The crash rows replay every prefix of every write sequence and require the check at the next mount to accept or refuse by the rule. The cache's rows cover the dependency check at all three points, the depth budget on both sides, and a cycle.
* `tests/run krffsmount`: boots that mount a KRFFS partition and a KRFFS file over a loop device, read and write them, unmount them and have the host walk them; a boot killed inside a write; two writers on one mount; the refusals; a FAT32 image and an ext2 image over the same loop device.
* The existing suites are what say the root did not change, and they run at the end of every phase that touches `krffs.c`.
* The machine: the ext2 root's image file over a loop device, and a KRFFS partition of the stick. The notebook's scratch partition is written only if the owner approves it at the time, under the disk rule; the milestone does not depend on it.

## Documentation

`rach2/sys/fs/krffs.Readme.md` is new: the format, the core, the two backends, the order of writes and what the check repairs. `vfs.Readme.md` lists `krffs` among the types `mount(2)` places. `block/Readme.md` gains the loop device, its lifetime and the dependency rule; the rule is restated where the cache's hold rules live. `krffs/Readme.md` records what the oracle now checks. The Readme's limits say what a mounted KRFFS volume may be.

## The phases

Every phase ends with `tests/run style`, `tests/run host` and the suites it touches. A Codex unit reviews the boundary marked in each of phases 2, 4 and 6 before the next phase starts.

### Phase 1: the oracle and the crash model

`fsck.krffs` strengthened; the update protocol and what the mount check repairs written down in the new Readme; host rows for the protocol over images, with the mutations.

Proof: the oracle refuses a chain whose backlink is wrong and one whose forward chain is broken, and accepts what the protocol can produce; `tests/run all` unchanged.

### Phase 2: the offset core

`krffs.c` in offsets over a backend, the memory backend only, the root on a pointer facade, the VFS adapter on encoded handles.

Proof: `tests/run all` passes unchanged, both roots boot, the host rows over the core pass with their mutations. Review at this boundary before phase 3.

### Phase 3: the block backend, read-only

The second backend; `mount(2)` accepts `krffs` with `MS_RDONLY`; the mount state pool and the lifetime operations; `/proc/filesystems` lists the type.

Proof: a boot mounts a KRFFS partition of a disk image, reads every byte of known files, unmounts; a bad magic, a short region and a malformed chain are refused by name.

### Phase 4: the block backend, writable

Writes through the cache under the mount lock, in the order the protocol fixes; the dirty bit; the check at the next mount with its repairs recorded.

Proof: a program writes files of every kind and unmounts; the oracle finds the volume consistent; the crash rows replay every write prefix and each mounts, repairs and reads what the rule allows; two writers on one mount interleave without tearing the chain. Review at this boundary before phase 5.

### Phase 5: the cache's dependency rule

The rank replaced by an exact dependency, checked where a buffer is taken, where a dirty victim is written and where a sync writes; the depth budget; the refusal of a cycle.

Proof: the cache's host rows, including a full cache with nested devices and the depth on both sides; the mutation that removes each check trips its own row.

### Phase 6: the loop device

`/dev/loopN`, the control node, `/proc/loop`, the attach and detach protocol, the utility, and the entries in the two tables.

Proof: a FAT32 image file and an ext2 image file on the ext2 root each mount through a loop device, take a file and give it back after an unmount, and read the same on the host; detach with a mount up is refused, and after the unmount it invalidates and releases; the refusals. Review at this boundary before phase 7.

### Phase 7: KRFFS through the loop device

A KRFFS image file mounted, read and written, on a FAT32 backing and on an ext2 backing.

Proof: the suite's rows and the oracle after each; a loop device over a file that lives on a loop device is refused by the depth rule.

### Phase 8: the machine

The notebook: a KRFFS image file on the ext2 root through a loop device, written, unmounted and checked on the host; a KRFFS partition of the stick mounted and written; the acceptance script's new lines.

Proof: both work on the machine, and every difference the machine shows is fixed in its subsystem with a row.

### Phase 9: the record and the sweep

The Readmes, the notes closed line by line, `tests/run all` on the final tree, the milestone's Readme with its trust statement, and the `dev-log/Readme.md` entry.

## Risks, in expected order

* The crash protocol. The chain's backlink cannot survive every prefix, so the design rests on the forward chain and a repair; if the repair cannot cover a state the protocol produces, the protocol changes, not the repair. Phase 1 settles it before any write is implemented.
* The offset core touching the root. It is phase 2, behaviour-preserving, with the whole suite as its proof and the pointer facade keeping the callers outside the file system unchanged.
* The cache's rule. A loop read takes another device's block while holding one, which the cache panics on today; the danger of the fix is a deadlock rather than a panic, so the rule is checked at all three points and the depth is computed rather than assumed.
* The write-through promise. A loop write that does not reach the backing file leaves the cache's contract broken and a volume that reports what it has not stored; the backing `fsync` is part of the write, and its failure is a device error.
* Scope. The root's window, its pointer callers and the format's growth are named out of scope; a finding that argues for them is recorded for a later milestone.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md` and `notes.md`. Run `git status --short --branch` and `git log --oneline -15`. The tree is the authority for what is built; this plan is the authority for what is decided.

## The goal prompt

`Run milestone 44, mounting KRFFS, from dev-log/44-krffsmount/plan.md.`
