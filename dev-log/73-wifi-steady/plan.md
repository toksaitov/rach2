# 73 wifi steady

Three small Wi-Fi fixes, run through the factory as a test of it. The GitHub milestone exists because `release.sh` needs one.

* Signal: `recv.c` and `xmit.c` add the noise level, not the raw floor, so a reading does not fall 21 to 23 dB at the first noise-floor result (#2408).
* Patience: wait 17 beacon intervals, 1740 ms, for a silent AP, with a temporary log line at each loss, which #2407 removes (#2412).
* Radio switch: an rfkill block closes the interface as Linux does, and an unblock rejoins by itself (#2416).
* The Lenovo proves the signal and the switch (#2421).

Skipped by the owner, this once: the release on the Mac and on Windows. The full run is WSL only, under TCG (`release.sh -o`). Left out by the owner: the 11 s `wpa_cli reassociate` and the small `iw` gaps.

Evidence: `build/flow/coordinator/lenovo-hotspot-20261104/findings.md`.

## One file's wait

The owner's order of 2026-11-06, after the disk load showed a slow request stopping every file on a mount: "Fix it now, fully". A slow disk request holds up only that file's users, metadata reads included.

Done means all of these:

* The disk load of the six `dd` writers and the `sync` loop on the host with hostapd's 3.5 s limit passes wifi 3 of 3 on both architectures.
* The longest hold of every lock across a disk wait is listed, and none outlasts one file's own request.
* A new case shows a slow request on one file leaving another file's users running.
* A full run in WSL under TCG on the final head, not `-r`.
* The Lenovo step as first ordered.

### What is true now

* One lock covers the kernel, a system call is not preempted, and the big lock goes while a thread sleeps. So anything changed between two sleeps is already atomic, and a sleeping lock is needed only for what must stay unchanged across a sleep.
* `vfs_ns_lock` is one such lock per mount. ext2 sets `ns_serialised`, so `fd_ofd_lock` holds it for a whole `read(2)` or `write(2)`, and locked walks hold it from the lookup to the change. It nests by depth, so the interim `read_fetch` of 8f45e5813 lets it go only on paths that took it once, faults and exec, and not under `read(2)`.
* Types without `ns_serialised`, FAT32 and exFAT, already show the other shape: the walk locks nothing and the callers take the mount's lock around each change (`if (!held) vfs_ns_lock(parent.mnt)`), some 25 sites.
* The writable block size is 4096, the cache's. A drain on a mount that writes back is copies into the cache (`bcache_get_blank`), with no read and no write but a dirty victim's.
* `user_buffer_ok` prefaults a user buffer before any lock is taken.

### What the reviews changed

Codex and a Fable subagent each reviewed a first design; their reports are in `build/flow/coordinator/m73-locks/`. Both found it incomplete, and each point below was checked against the code. Taken: staged or checked user copies, canonical held nodes in place of `node_held`, ancestry order for directories, three lifetime states of a node, per-punch state, a real drain of deferred puts, a shared rule for mount and removal, admission closed before a remount, the guard in `inode_write`, the drain's victim writes, ownership rules for shared blocks, bitmaps on fsync, and interleaving tests. Set aside: removing the batch altogether (Codex), which rewrites its crash matrices for no gain in the Done list; keeping one lock for the namespace (Fable), which leaves a directory's miss stopping other files, against the order. A device's single active command and its flush are shared by nature; no lock changes them.

Codex also found what is not a lock at all: a page fault reads a pollable disk with interrupts off and the big lock held (`filemap_read`, `transfer` in `request.c`, `ahci_drain`), so the whole kernel stands still for that request. #2430 traces it first.

### Design

The design is V6 Unix's: a sleeping lock on each inode, a short allocator lock and the cache's own busy buffers. ext2 stops setting `ns_serialised` and sets `vfs_type.file_locked`. KRFFS, tmpfs, FAT32 and exFAT behave as they do now.

| Lock | Scope | Covers | May wait on the disk for |
|---|---|---|---|
| `em->rename` | mount | ancestry of a directory's move to another parent | the walk up `..`; it stops only other such moves |
| node lock | one inode | its attributes, block map and a directory's entries | that file's own requests |
| `em->meta` | mount | the batch, bitmaps, descriptors, free counts | one request, and that only when a block fetched for it was evicted again |
| buffer | one block | the cache's rules, unchanged | one request |

Order: `em->rename`, then directories, then non-directories, then `em->meta`, then buffers.

* Related directories go ancestor first, decided by `is_below` under `em->rename` before either lock is taken. Unrelated directories go by inode number. Children go after every directory, by inode number, and a rename's source and target too. A walk holds nothing while it asks.
* `struct sleep_lock` moves out of `vfs.c`: place in line, depth, holder, and the longest hold across a disk wait. The mount's lock and the node's are both one. Each process counts the file locks and `em->meta` it holds (`file_locks`), which `vfs_ns_held_by_caller`, `vfs_ns_held_other` and the cache's `victim_ok` read, so that reclaim under any of them cannot re-enter a file system.

User copies:

* `user_buffer_ok` holds only until the caller sleeps. `vm_resident(addr, len, write)` walks the page tables and never faults. A copy to or from a user buffer under any file lock runs in the same step as a `vm_resident` that passed, with no sleep between. When it fails the operation lets its locks go, prefaults, takes them again and goes on from the same byte. Under the big lock nothing can unmap the range inside that step. `writeback_commit` checks before it changes anything, so the batch never stops half committed. A buffer holds no file lock it can fault under: a copy out of a held buffer goes the same way.

A node's life:

* States `N_LOADING`, `N_LIVE` and `N_DYING`. `node_for` publishes a node `N_LOADING` before it reads the inode. A second caller takes a reference and sleeps on `em->node_wq`; a failed load marks the node failed and wakes them, and the last reference frees it. `N_DYING` takes no new reference: a caller sleeps until the node has gone and looks again.
* `inode_alloc`, under `em->meta`, skips a number that is in the hash, so that nothing there waits for a node. `inode_new` publishes its node before `em->meta` goes, so `inode_write` and `node_new` have no gap.
* `node_held` goes. `unlink`, `rmdir`, `rename` and `link` work on a held canonical node with its lock, never on a borrowed pointer or a copy on the stack. `rmdir` keeps the child's lock through its emptiness check and sets `unlinked`; `dir_add` refuses an unlinked directory under the same lock.
* `ext2_put`: a reference is dropped between sleeps. The last put of an unlinked inode marks it `N_DYING`. A caller with `file_locks` zero frees it there; any other leaves it on `em->reap` with a hold on the mount (`vfs_enter`). `em_reap` runs at the end of every operation of ext2, when the caller holds nothing, in `ext2_sync`, and in `ext2_prepare_unmount` before its sync, so nothing outlives the mount.
* A remount closes admission (`em->closing`), so that new writers wait, then waits for `em->writers`, counting batch nodes, final puts and mapped writeback, holding no lock they need, then scans for unlinked nodes and flips. An error flip sets the read-only flags and waits for nobody.

The batch and the allocator:

* A write first fetches what it needs under the node lock alone: the pointer path, the data block of a partial write, the bitmap and descriptor of the hint's group. Then `em->meta`, the plan and the commit. A read inside `em->meta` that misses returns -EAGAIN before anything has changed, under a planning flag. The piece rolls back its images in the same step, lets `em->meta` go, fetches and starts over; after eight tries it reads under the lock, one request.
* `write_bytes` patches the batch's image of its block in the same step as the buffer. `read_bytes` looks in images first. So `inode_write`'s guard goes: an inode the batch holds is stored from the node, whose pointers may name blocks not yet in the cache. That is sound on a mount that writes back, which promises no order, and a strict mount drains at the end of every call. Namespace operations no longer drain.
* A drain calls `bcache_reserve(dev, n)` with nothing held, which writes dirty victims until `n` buffers can be had without a write. Then `em->meta` and the copies. A batch node's put moves to `em->reap`. `em_balance` runs after `em->meta` goes.
* `punch` has two phases under the node lock. It clears up to 512 pointers, dirty first, collecting their blocks and the blocks of emptied pointer blocks in a private list, then frees them group by group: fetch the bitmap with nothing else held, then `em->meta`, a drain when a batch is pending, the bits, dirty after. The order is the cache's two classes, so overlapping punches need no shared table; `em->batch`, `meta[]` and `meta_deferred` go. A strict mount syncs between the phases.
* A strict mount holds `em->meta` across its ordered writes, as that option asks. This is a listed exception.

Reads and the VFS:

* `ext2_read`: the node lock for the map and the copy, let go around a data block the cache lacks and around the pointer blocks that lead to it. `fd_ofd_lock` takes no file lock. `getattr` takes none. A write takes the node lock around the size read of `O_APPEND` and the write itself, in `fs_write`, and mapped writeback takes it for its size check and write. A truncate takes it through `filemap_file_truncated`.
* `vfs_node_lock(ref)`: the node's lock on a `file_locked` type, the mount's otherwise. The sites that take the mount's lock around a change move to it, some 25.
* A locked walk on a `file_locked` type holds nothing, like FAT32's. The caller locks the parent and rechecks under it what it checked before: search and write permission, the name's presence, mounts and roots, trailing slashes, `RENAME_NOREPLACE`. The target's lock is held for sticky, immutable and append checks.
* `rename`: `vfs_rename_lock` resolves both parents with nothing held, then takes `em->rename` when they differ, orders them, and looks both names up again.
* Mount against removal: `mount_place` and `covered_already` run under the covered directory's node lock, and `rmdir` and a rename over a directory check for a mount under the victim's. The mount table itself stays on the mount's lock.

Per-file fsync:

* `bcache_dirty_owned(b, owner)` tags a block only one file uses, its data and pointer blocks, with the inode; a shared block keeps owner 0. A stale tag costs one needless write and nothing else. `bcache_sync_owned(dev, owner)` writes that owner's dirty buffers in runs that hold only its blocks, without the flush.
* A node keeps up to 8 shared blocks dirtied for it (`n->shared[]`): its table block, bitmaps, descriptors. An fsync drains the batch, writes the owned blocks, then the shared ones, firsts of the whole device before a shared block of the after class, then flushes. Past 8 it falls back to the device's `bcache_sync`. So a crash after an fsync leaves no bitmap that calls a used block free, which this driver's own check would mount read-only.
* `n->dirty_meta` is set by a change of size, pointers, mode, owner or links, and not by times. `fdatasync` skips the table block when it is clear. `vfs_ops` gains a nullable `fdatasync`, and both system call tables stop routing it to `sys_fsync`.
* FAT32, exFAT and KRFFS volumes use the same call for their clusters and their maps.

The record: `/proc/fs/lockwait` lists each kind of lock's longest hold across a disk wait and who held it. A proc counts its finished disk waits; a lock remembers the count when taken, and a hold during which it moved is one that crossed a wait.

Tests: a nested interleaving harness in `ext2_hosttest`: at each disk read or write of one operation the hook runs a second operation as another `current_proc`, which must finish or must report that it would block, by name, in a table of rows for every pair that matters. A machine case on the lag device for the Done list. A machine stress of creates, renames, unlinks, truncates, reads and writes from several processes under memory pressure, with `e2fsck -fn` after. A strict-mount crash matrix stays as it is.

### Tasks

Each is an issue with its own Done. The order is what must land first; everything else runs at once. ext2.c's tasks work on its separate regions and rebase often.

* #2430: every wifi failure under disk load, the polled page-in first. Not the locks.
* #2431: the Lenovo soak and its baseline.
* L1: `sleep_lock`, `file_locks`, the wait record, `vm_resident`, `file_locked`. No change of behaviour. First, and small.
* L2, on L1: `vfs_node_lock`, the walk, rename's lock, mount against removal, `fs_write` and mapped writeback, about 25 sites. Owns `vfs.c`, `vfs.h`, `fd.c`, `syscall_fs.c`, `syscall_proc.c`, `filemap.c`, `loop.c`, `ipc/unix.c`, `dcache.c`.
* E1, on L1: ext2's skeleton: `em->meta`, `em->rename`, node states and helper names, no change of behaviour. Small, and the other E tasks wait for it.
* E2, on E1: a node's life: `node_for`, `ext2_put`, `em_reap`, `inode_alloc`, remount. Region: the top of `ext2.c`, `ext2_remount`, `release_inode`.
* E3, on E1: the batch and allocator: fetch before `em->meta`, -EAGAIN, `write_bytes`, the drain, `bcache_reserve`, `punch`. Region: `write_bytes` to `writeback_contiguous`.
* E4, on E1: reads and writes under the node lock with `vm_resident`, and the namespace operations on held nodes. Region: `ext2_read`, `ext2_write`, `dir_*` to `ext2_rename`.
* E5, on L2, E2, E3, E4: flip the type's bits; the harness's rows.
* F1: `bcache_dirty_owned`, `bcache_sync_owned`, `bcache_reserve`, `fdatasync` in `vfs_ops` and the system call tables. Now. Owns `bcache.c`, `bcache.h`, `bcache_hosttest.c`, the tables.
* F2, on F1, E3: ext2's fsync and fdatasync.
* F3, on F1: FAT32 and exFAT. F4, on F1: KRFFS.
* R1, on L1: `/proc/fs/lockwait`, the lag case, the stress case and their pages.
* The end: the Done list, in order: the disk load 3 of 3, the record, `release.sh -p`, the full run in WSL, the Lenovo soak on the installed final image.

The Wi-Fi issues are finished when the soak is clean and the full WSL run passes on the same head.
