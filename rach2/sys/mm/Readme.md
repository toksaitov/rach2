# Rach2 Memory Management

Physical frames, a virtual page table, a per-process address-space map (VMAs), and a demand-paging fault handler that backs heap, stack, and anonymous `mmap` memory lazily. Together they let a real libc allocator run: a musl program can `malloc`, `qsort`, grow its stack, and fault cleanly, while a wild pointer kills only the offending process and leaves the kernel running.

## Pieces

| File          | Responsibility                                                                                                     |
|---------------|--------------------------------------------------------------------------------------------------------------------|
| `frame.*`     | Size the physical frame pool from the multiboot memory map, capped to the direct-mapped window                     |
| `paging.*`    | 32-bit page tables, the identity map, user-page alloc/free, and the page-fault dispatcher                          |
| `pagecache.*` | The page cache plus its demand-paged disk window at `PCWIN_BASE`, with CR0.WP dirty tracking and writeback         |
| `vm.*`        | The VMA model: a small fixed array of `{start, end, prot, type}` regions per process, with find/insert/remove/grow |
| `heap.*`      | The kernel's own bump and region allocator (`mem_reserve`) for kernel structures                                   |
| `usercopy.*`  | The `copy_from_user`, `copy_to_user`, and `strncpy_from_user` boundary, fault-aware                                |

## Address-space model

Every process carries a `vm_space_t` (in its PCB) describing its user memory as a handful of virtual memory areas. Each VMA is a half-open, page-aligned range `[start, end)` with a protection (`VM_PROT_*`) and a kind (`VM_CODE`, `VM_DATA`, `VM_HEAP`, `VM_STACK`, `VM_ANON`). This is the structure real kernels build on (Linux `vm_area_struct`, BSD `vm_map_entry`); here it is a 16-entry array, since a teaching process has only a few regions. `vm.c` is pure logic with no kernel globals, so the host seam test (`tests/host/vm_hosttest.c`) exercises find, insert, remove, overlap, and stack growth directly under ASan and UBSan.

`exec` builds the map (`elf_setup_vmas`): one VMA per loaded ELF segment with the segment's real permissions, an empty heap just past the image (`brk == image_end`), and a one-page stack at the top growable down to `stack_limit`. `fork` copies the parent's map verbatim.

## Eager versus lazy

ELF code and data segments are loaded eagerly at `exec` (their pages are mapped up front), which keeps the loader simple and the boot path unchanged. Everything that grows at runtime is demand-paged: heap pages (`brk`), the stack below its first page, and anonymous `mmap` regions. The 32-bit x86 page tables have no NX bit, so "code" is present, user, read-only and "data", "heap", and "stack" are present, user, writable; only the write bit distinguishes them, which is what copy-on-write keys on.

## Demand paging

`_page_fault_handler` (vector 14) reads `CR2` and the error code, then tries each case in turn.

* Copy-on-write: a write that faulted on a *present* user-space page may be a frame `fork` left shared read-only. `cow_fault` runs first, since `vm_resolve` would see the page already present and loop; it copies the frame, makes the page private and writable, and the instruction retries. A present-page write that is not COW falls through to the read-only segfault below.
* Demand paging: a fault on a user-space address (`addr < USER_ADDR_MAX`) of a live process goes to `vm_resolve`. If a VMA covers it (or it is a legal one-page-or-more stack growth below the stack VMA and at or above `stack_limit`), a fresh frame is allocated, zero-filled, and mapped; a write to a read-only region or an address in no VMA fails. This keys on the faulting address being in user space, not on the CPU mode, so a kernel touch of a lazy user page resolves here too.
* Page-cache window: a supervisor fault inside the demand-paged disk window (`addr >= PCWIN_BASE`, below `PCWIN_BASE + pc_window_size()`) goes to `pc_fault`, which polls the backing sectors into a cache frame and maps it. The window is a kernel-only mapping the file system touches in ring 0, so the branch is gated on `!user_mode`: a ring-3 touch of a window address is a wild access that falls through to the segfault. A window fault past the backed size, or one the fill cannot serve, is a real bug and panics.
* An unrecoverable fault taken in user mode segfaults the process. `terminate_current_process` (in `kernel.c`, shared with the CTRL+C path) tears it down and schedules the parent to collect the zombie; if the dier was a real init it panics instead (a rescue/no-init debug shell regains control). The machine survives.
* A fault in kernel mode with no valid user cause is a genuine kernel bug, and panics loudly.

Newly demand-paged pages are always zeroed, so heap, stack, and `mmap` memory reads as zero and a recycled frame never leaks a previous process's data.

## Syscalls

* `brk` (45): the heap is a single demand-paged `VM_HEAP` VMA `[heap_start, page_up(brk))`. Growing extends the region (pages fault in lazily); shrinking frees the pages above the new break. Returns the resulting break, matching musl's `__brk` contract.
* `mmap2` (192): anonymous private mappings only (what musl's allocator uses), placed by bumping `mmap_top` down from a fixed base between heap and stack. Pages are demand-paged and zero-filled; the file-backed path returns `-ENOSYS`.
* `munmap` (91): drops the VMA bookkeeping (trimming or splitting partial overlaps) and frees the backing frames.

## Fault-aware usercopy

Because user memory is now demand-paged, the kernel must not dereference a not-yet-present user page directly. `copy_from_user`, `copy_to_user`, and `strncpy_from_user` range-check the pointer and then pre-fault each page through the same `vm_resolve`: a valid-but-lazy page is backed (with write permission for copies into user space), and a page outside every VMA returns `-EFAULT` instead of faulting in kernel mode. `strncpy_from_user` faults one page at a time as it crosses boundaries, since the string may end before a later, legitimately unmapped page.

## Page cache and the disk window

`pagecache.*` lifts KRFFS off the in-memory RAM module onto a real disk. `init_pagecache` reserves a fixed pool of `PC_SLOTS` (256) frames, one MiB, and pre-maps the page tables for a kernel-virtual window whose base is `PCWIN_BASE` (0xC0000000, equal to `USER_ADDR_MAX`: just above user space and below the PCI MMIO hole). The window span is the page-table coverage already laid down, sized against the lowest device MMIO BAR and capped at `PC_WINDOW_CAP` (256 MiB). `pc_set_backing` points the cache at a backing store (sized to the partition) and starts it empty; the file system then reads and writes the backing simply by touching window addresses.

A window fault is serviced by `pc_fault`: it polls the backing sectors into a free or evicted cache frame and maps it, read-only on a read and read-write on a write. The fault path polls and never sleeps, so it runs with interrupts off. Dirty tracking leans on CR0.WP (set alongside PG at `init_paging`): a clean frame is mapped read-only even for the file system in ring 0, so the first supervisor write faults again and `pc_fault` marks the frame dirty and re-maps it writable. `pc_sync` (fsync and sync) writes back every dirty frame and re-protects it read-only; eviction writes a dirty frame back before reusing it.

The backing store is one page at a time, behind the `pc_read_fn`/`pc_write_fn` pair the mount installs. For a disk-backed file system that pair drives the block layer in `sys/block`: `block.*` is a sector-addressed device behind a small read/write vtable (with a partition view that adds `start_lba`), and `ata.*` is the polled PIO ATA driver. Reads and writes there are synchronous and polled too, so the window fault can drive them with interrupts off and the scheduler stopped.

## Testing

* `tests/vmprobe`: brk grow and shrink, heap read and write, anonymous `mmap` map/read/write/`munmap`, deep recursion to force stack growth, and the lazy-pages-read-zero check. Prints `VMPROBE OK`.
* `tests/vmfault`: writes through a wild pointer into an unmapped gap; the process must die with the kernel surviving and the shell returning.
* `tests/musl/mallocqsort`: a musl program that `malloc`s, `qsort`s, and makes a 256 KB (mmap-backed) allocation. Prints `MALLOC QSORT OK`.
* `tests/host/vm_hosttest` and `tests/host/frame_hosttest`: native ASan and UBSan seam tests for the pure VMA and frame-sizing logic (`make verify-host`).
