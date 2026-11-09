# Shared buffers and fences

`sys/dmabuf/` shares buffers and completion fences between devices and processes through Linux's dma-buf, sync-file and udmabuf interfaces. `/dev/udmabuf` exports pinned ranges of sealed tmpfs files. A software timeline supplies fences for tests through `sw_sync_open`, which debugfs binds to `sync/sw_sync`. The user structures and ioctl numbers are in [`../drm/uapi.h`](../drm/uapi.h), and the kernel interfaces in [`dmabuf.h`](dmabuf.h) and [`../drm/drm.h`](../drm/drm.h).

## Use

Open `/dev/udmabuf` and call `UDMABUF_CREATE` with a memfd, a page-aligned offset and a page-aligned size, or `UDMABUF_CREATE_LIST` with ranges to concatenate. Each file must have `F_SEAL_SHRINK` and lack `F_SEAL_WRITE`. The resulting read/write descriptor maps the same pages shared, reports its size through `lseek(fd, 0, SEEK_END)` and `fstat`, and has an inode distinct from other exports. `/proc/PID/fd` names it `anon_inode:dmabuf`. [udmabuf(4)](../../../man/man4/udmabuf.4) and [dma-buf(7)](../../../man/man7/dma-buf.7) give the user contracts.

## Interfaces

* `dma_fence_init` starts a fence with one reference, a context and a sequence number. `dma_fence_signal` records monotonic time, runs each pending callback once and wakes the fence's queue. `dma_fence_set_error` records a negative producer error before completion. `dma_fence_status` returns zero while active, one on success or that error. `dma_fence_wait_until` holds a reference across sleeps, with an absolute monotonic deadline or a negative value for no deadline; it returns zero on completion, `-ETIME` on expiry or `-EINTR` for an interrupted wait. The producer error is read separately.
* `dma_fence_merge` builds a flattened all-of fence, keeping the latest point of each context and propagating a producer error after all children complete. A zero-input merge gives a completed stub, and a single leaf keeps that leaf. A merge owns its children and withdraws their callbacks when released. Names come from `dma_fence_ops`; deadline hints reach every leaf.
* `dma_resv_add_fence` adds a read fence or joins outstanding writers on a write, taking a reference of its own. `dma_resv_snapshot` gives a reader the outstanding writers, and a writer the outstanding writers and readers, as one referenced fence. `dma_resv_fini` releases every slot. The caller holds the big kernel lock and orders submission against the work the snapshot names.
* `dma_buf_export` takes exporter storage on success and returns one reference. Its optional external reservation must outlive the buffer. `dma_buf_fd` installs another reference and leaves the caller's alone; `dma_buf_get` finds a descriptor with a new reference, `dma_buf_ref` takes one directly and `dma_buf_put` releases one. The exporter's `mmap` attaches backing with a buffer reference for the mapping, and `release` returns exporter storage. Its optional `page` hook lends physical frames to device importers, and `map_flags` records their cache type. `read_only` selects read-only descriptors; its default of zero keeps udmabuf read/write. Its optional `cpu_access` hook receives validated DMA_BUF_SYNC flags after the start's waits.
* `sync_file_create` installs a close-on-exec sync file with its own fence reference. `sync_file_get_fence` returns a reference or NULL. `SYNC_IOC_MERGE` omits completed leaves as Linux does and keeps a completed stub when nothing is pending. `SYNC_IOC_FILE_INFO` counts or copies flattened leaves. Poll watches the fence's own queue and reports POLLIN on completion, including a producer error, which FILE_INFO reports.
* `sw_sync_open` attaches a new timeline to a just-opened descriptor. CREATE_FENCE returns a sync file, INC advances the 32-bit timeline with signed wraparound comparisons, and close completes outstanding points with `-ENOENT`. GET_DEADLINE reads a hint supplied through the sync file, or returns `-ENOENT` when none was supplied.

## Design

Every operation runs under the big kernel lock. A wait, a copy to or from user memory and an exporter operation may sleep; a completion callback may only signal, wake or queue work. The ownership and device-loss rules in [`../drm/drm.h`](../drm/drm.h) apply to every exporter and fence producer.

Udmabuf takes a frame reference for each page before publishing a descriptor. The frame remains pinned after the memfd closes. A shared device mapping holds the dma-buf, and its last region releases that reference at unmap, exec or exit; fork shares the mapping's backing record. Each buffer poll arms callbacks on a reservation snapshot; completion wakes the buffer's queue, and a later poll reads the reservation again. Explicit sync-file export takes a snapshot which later imports cannot change.

## Limits

Udmabuf uses Linux's default bounds of 1,024 ranges and 64 MiB per export. It supports tmpfs files; Rach2 has no hugetlbfs files. Shared mappings use cached RAM and need no cache maintenance on x86. An exporter for other memory supplies its own mapping and CPU-access hooks. The software timeline operations are available through `sw_sync_open`; debugfs does not yet publish `sync/sw_sync`.

## Tests

`tests/host/fence_hosttest.c` drives fences, context ranges, callback removal, timed and interrupted waits, flattened merges and errors, reservation snapshots, buffer size and inode identity, sync-file export, merge, info and import, buffer poll, software timeline completion, deadline hints and producer close, under the sanitizers. Its descriptor table and scheduler are models; the fence and buffer code is included whole.

`tests/udmabufprobe.c`, the `dmabuf` suite, runs the real descriptor operations on both architectures: rejected inputs, distinct inodes and size, shared pages and reversed list ranges, fork, export, merge, info, import, CPU access and poll, then pages retained after all descriptors close. It releases every descriptor and mapping on either exit. The checkpoint and release run the consumer tests named in the milestone plan.
