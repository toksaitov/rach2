# Memory management

The memory subsystem manages the kernel's 32 MiB physical window, per-process page tables, virtual memory areas (VMAs), copy-on-write fork, demand paging, checked user access, and the disk page cache. Pages are 4 KiB.

## Physical memory

`frame.c` reads the Multiboot memory map and takes the highest end of an available region. It falls back to `mem_upper` when no usable map is present, then caps the result at `HARD_MEMORY_LIMIT` (32 MiB). `init_paging` creates one reference-count byte per managed frame. Zero means free; positive values count mappings of a shared frame.

The kernel identity-maps `[0, HARD_MEMORY_LIMIT)` as supervisor-writable pages. Only frames within the size reported by `frame_usable_count` enter the allocation pool. `mem_reserve` is the kernel's monotonic boot allocator. Its initial break is the greater of `_end` and the highest Multiboot module end. It skips frames already allocated after paging starts and fails before crossing the memory limit.

Before reserving module memory, `modlayout.c` rejects an empty or backwards range, a module past 32 MiB, a module overlapping the kernel, or two overlapping modules. The rejection includes the module number and physical range.

`alloc_user_page` scans upward from the end of the boot reservations. Page-cache frames are reserved from the top of the pool downward. User frames return to the pool when their reference count reaches zero; page directories and private page tables use separate free lists.

## Page tables and address spaces

`init_paging` creates the kernel page directory, enables paging and CR0.WP, and registers the vector 14 handler. CR0.WP makes supervisor writes honor read-only PTEs, which the page cache uses to detect its first write to a clean page.

A process page directory initially copies the kernel directory's PDEs. The referenced kernel page tables remain shared, while a user mapping first copies or allocates the page table for that process. A context switch loads the process's directory into CR3. Kernel page tables needed by every process, including the page-cache window, must exist before the first directory is cloned.

User addresses run from `USER_ADDR_MIN` (`0x02000000`) through, but not including, `USER_ADDR_MAX` (`0xC0000000`). The kernel may identity-map selected MMIO regions outside the low physical window. Creation of a shared kernel PDE fails after the first process directory has been cloned.

## Virtual memory areas

Each PCB contains a `vm_space_t` with at most 512 VMAs. A VMA is a half-open, page-aligned range with `VM_PROT_READ`, `VM_PROT_WRITE`, and `VM_PROT_EXEC` bits and one of these types:

* `VM_CODE` and `VM_DATA` describe loaded ELF segments.
* `VM_HEAP` describes the non-empty part of the `brk` heap.
* `VM_STACK` starts with one page and may grow downward by at most 8 MiB.
* `VM_ANON` describes an anonymous `mmap2` allocation.

`execve` loads ELF pages eagerly, then creates one VMA per loadable segment with the segment's permissions. It records an empty heap at the image end, places the initial stack at `0xBFFFF000`, and starts anonymous allocation below `VM_MMAP_BASE` (`0xB0000000`). The heap receives a VMA on its first non-empty growth. `fork` copies the complete `vm_space_t`.

The syscall contracts are:

* `brk` (45) grows or shrinks one heap range. Growth reserves virtual addresses only; pages are allocated on first access. Shrinking releases present frames. A collision or out-of-range request returns the old break.
* `mmap2` (192) requires `MAP_ANONYMOUS`, rejects `MAP_FIXED`, ignores the address hint, file descriptor, and page offset, and allocates downward from `mmap_top`. File-backed mappings return `-ENOSYS`. The implementation does not validate `MAP_PRIVATE` against `MAP_SHARED`; every accepted mapping receives the private copy-on-write behavior used by fork.
* `munmap` (91) removes full VMAs, trims either end, or splits a VMA, then releases every present frame in the range. A never-faulted page has no frame to release.

VMA allocation does not reserve physical frames. If a later user fault cannot allocate a frame, the unresolved fault terminates that process with `SIGSEGV`. A kernel prefault reports failure to its caller instead.

## Page faults

The vector 14 handler reads CR2 and the x86 error code, then handles these cases in order:

1. A write protection fault on a present user COW page calls `cow_fault`.
2. A user-range fault for a live process calls `vm_resolve` for an existing VMA or legal stack growth.
3. A supervisor fault inside the backed page-cache window calls `pc_fault`.
4. Any remaining user-mode fault terminates the process with `SIGSEGV`; a real init death halts the kernel.
5. Any remaining supervisor fault panics the kernel.

`vm_resolve` checks the VMA permissions. A new heap, stack, or anonymous page receives a zeroed frame. A read-only VMA remains read-only in its PTE. A write prefault of a present COW page makes the page private before kernel code writes through the pointer. A present page that cannot satisfy the requested access is rejected instead of retrying the same instruction.

Stack growth extends the stack VMA down to the faulting page when the page is at or above `stack_limit`. Growth fails if it would cross another VMA. Pages between the old and new start remain unallocated until touched.

## Copy-on-write fork

`fork` maps each present user frame into the child. Writable pages become read-only and receive `PTE_COW` in both processes; the frame reference count increases. Genuine read-only pages keep their existing flags and are not marked COW.

On the first write, `cow_fault` allocates and copies a frame when another process still references the old one. If the writer is the sole owner, it removes `PTE_COW` and restores write permission without copying. Exit, `execve`, `munmap`, and failed-fork cleanup decrement the same reference counts.

## Checked user access

`copy_from_user`, `copy_to_user`, and `strncpy_from_user` reject addresses outside the user range. They call `vm_prefault_range` before dereferencing user memory, so a valid lazy page is created and an unmapped or read-only destination returns `-EFAULT` instead of faulting in supervisor mode. `strncpy_from_user` checks one page at a time because the terminating NUL may appear before the next unmapped page.

The kernel also prefaults ranges before passing user buffers to code that accesses them directly. A write prefault resolves COW before any part of the operation has modified the buffer.

## Page cache

The page cache maps a backing store into a supervisor-only window at `PCWIN_BASE` (`0xD0000000`). The window ends at the lowest 32-bit PCI memory BAR within it, or at the 512 MiB cap when no BAR intervenes. Its page tables are allocated before the first process starts.

At boot, the cache reserves `min(1024, free_frames / 4)` frames. The boot stops if that result is below 256. The reserved capacity is therefore 1 MiB through 4 MiB. Each resident page has a hash entry, a clock-replacement state, and dirty, accessed, and pinned flags.

`pc_set_backing` installs page-sized read and write callbacks. `rach2.fswindow` uses a Multiboot module as the backing store. A disk root uses the selected ATA or AHCI block device through an MBR partition view.

On a miss, `pc_fault` reads one page into a reserved frame. A read maps it read-only. The first supervisor write faults again, marks the page dirty, and remaps it writable. A write miss performs the read and marks the new resident page dirty immediately. A user-mode access to the window is never served.

Clock eviction writes a dirty victim before reuse. A failed write leaves the page dirty and resident. `pc_sync` writes every dirty page and re-protects it read-only; failed pages remain dirty for a later retry. `fsync` and `fdatasync` call `pc_sync` for the whole cache, while `sync` calls it but returns zero even after a writeback failure.

## Files

* `frame.*` sizes the managed physical-frame pool.
* `heap.*` implements the boot-time physical allocator.
* `modlayout.*` validates Multiboot module placement.
* `paging.*` owns page tables, frame references, faults, demand paging, COW, and kernel mappings.
* `vm.*` implements VMA lookup, insertion, removal, overlap checks, and stack growth.
* `usercopy.*` implements the user-pointer boundary.
* `pagecache.*` implements the disk window, residency, eviction, and writeback.

## Verification

`make test` runs `vmprobe`, `forkro`, `cowregrant`, `usercopyfault`, and `musl/vmamusl` in its default guest suite. They cover lazy heap and anonymous pages, stack growth, read-only mappings across fork, COW re-grant, checked strings crossing into an unmapped page, 300 simultaneous mappings, and at least 8 MiB of live small allocations.

The boot self-tests exercise page-table isolation and force page-cache faults, eviction, cross-page writes, and writeback through a RAM backing store. `tests/run host` runs the pure VMA, memory-map, module-layout, and page-cache bookkeeping tests under AddressSanitizer and UndefinedBehaviorSanitizer.

The test image also contains `vmfault`, `vmro`, `winprobe`, `cowprobe`, `musl/mallocqsort`, and `musl/cowfork` for dedicated or manual runs. See the [guest test inventory](../../../tests/Readme.md) for their execution contract.
