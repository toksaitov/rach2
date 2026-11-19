# A 4 KiB virtio-blk write under QEMU on WSL

Task 5342 traced the 5 to 10 ms that one 4 KiB virtio-blk write took during `machine.browser.chromium-webgpu` on the testing host's WSL, a figure from the release of milestones 13 to 21, and decided whether a block ext2 takes for a directory needs the `after` mark that made the disk flush before it.

## The write

The time is the host's `fdatasync`, which QEMU makes for the FLUSH that Rach2 sends after each write that does not carry `BLOCK_REQ_NOFLUSH`. The write itself costs about 0.1 ms.

* The case's root is a qcow2 overlay with 64 KiB clusters over the GNU image, on virtio-blk with QEMU's default cache mode, writeback (`tools/etude/qemu_profile.c:1043`, `tests/cases/machine.browser.json:1166`). The device then offers a volatile write cache, and Rach2 makes the disk `BLOCK_WRITE_THROUGH`: a write without `BLOCK_REQ_NOFLUSH` is answered only after a FLUSH that follows it (`vblk_answer` and `vblk_flush_half`, `rach2/sys/block/virtio_blk.c`), two requests, each one kick and one interrupt, the caller woken by the second's interrupt (`req_wake_caller`, `rach2/sys/block/request.c`). QEMU turns each FLUSH into `fdatasync` of the image file.
* Measured on the testing host on 2026-11-19 at a load average of 4 to 6, with `qemu-io -t writeback` on the same stack, a fresh overlay over a 512 MiB raw base, 100 operations each, every syscall of QEMU's traced with `strace -f -T`:

| Operation | Wall per op | `fdatasync` per op | Other syscalls per op |
|---|---|---|---|
| 4 KiB write into a fresh cluster, no flush | 0.29 ms | 0.05 | 1 `pwritev` of 64 KiB, 1.1 `pread64` of 64 KiB from the base |
| 4 KiB write into a fresh cluster, then flush | 5.25 ms | 2.03 (5.03 ms) | the above, 2 `pwrite64` of 64 KiB: a refcount block, then an L2 table |
| 4 KiB write into an allocated cluster, then flush | 1.70 ms | 1.00 (1.64 ms) | 1 `pwrite64` |
| 4 KiB write into an allocated cluster, no flush | 0.09 ms | 0.01 | 1 `pwrite64` |
| 4 KiB writes in sequence through fresh clusters, then flush | 1.99 ms | 1.10 (1.95 ms) | 1.1 `pwrite64` |
| raw image, 4 KiB write, then flush | 3.48 ms | 1.00 (1.67 ms) | 1 `pwrite64` |
| raw image, 4 KiB write, no flush | 0.09 ms | 0.01 | 1 `pwrite64` |

* The first write into a cluster of the overlay copies the cluster from the base and allocates it. At the guest's flush, qcow2 writes the refcount block and flushes the file before it writes the L2 table, so that no L2 entry on the medium names a cluster whose refcount is not there, and then makes the flush the guest asked for: two `fdatasync` calls for one 4 KiB write. The trace shows the order: `pread64` of the base cluster, `pwritev` of 64 KiB, `pwrite64` at the refcount block, `fdatasync`, `pwrite64` at the L2 table, `fdatasync`.
* A bare `fdatasync` after a 4 KiB `pwrite` on the WSL root, whose image is `D:\WSL\Ubuntu-26.04\ext4.vhdx`, took 3.18 to 3.58 ms, median 3.31, in the same hour, and 1.3 to 3.3 ms inside QEMU's traces. Review 6 of the release measured the flushes before `after` blocks at 1 to 6 ms each under the release's load. Two such calls at that load make the 5 to 10 ms.
* Not timed: Rach2's own time from the answer of the write to the issue of its flush, and from the flush's interrupt to the caller's wake, which no guest boot measured here. The code takes no sleep or tick in that path: the answer offers the flush from the interrupt (`vblk_vq_done`) and the flush's answer wakes the caller.

Nothing in the write's time is Rach2's to fix but the number of writes that carry the flush. QEMU's cache mode is the one a disk with a volatile cache has, and `cache=unsafe` would hide every flush.

## The `after` mark of a new block

A block ext2 takes for a directory, a symlink's target or a pointer block was written zeroed and marked `after` (`shared_block_new` in `ext2_dir.c`, `map_zero` in `ext2_file.c`), so that its write waited behind every `first` block of the device, the pointers and inodes that frees cleared, with the disk flushed between when a first had been answered without the flush. That put the flush before each mkdir's block; Chromium's GPU process makes 52 directories at its start (`upstream.md`, the WebGPU adapter entry).

The mark guards nothing. The allocator sets no bit in a bitmap block that holds a free's cleared bits not yet on the device: it writes that block first, with the group's lock let go, and looks again (`bit_turn` and `group_take_block` in `ext2_alloc.c`). The bitmap is an `after`, so its write goes behind the `first` blocks it follows (`write_follower` in `bcache.c`), with the flush between unless every first answered without the flush is covered by a flush that began after its answer (`order_flush`). So when a block is taken, every pointer and inode that named it in its last life is on the medium, and no old pointer there can name the new block. The file-write batch keeps the same rule when it copies its bitmap images into the cache (`ext2_bitmap_turn`), before its data.

The zeros are now written plain, still at once and before anything names the block, without the flush (ffb95f37b3). The writes of a bitmap that holds clears before a set keep their flush, which the order of a free needs.

The row `reuse_row` of `tests/host/ext2_hosttest.c`, which grows a directory into blocks a truncate freed while another process reclaims the cache at each read, and requires the truncated file's table block on the device before any freed block, now proves the bitmap's write as the guard.
