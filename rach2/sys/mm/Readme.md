# Rach2 Memory Management

Physical frames, a virtual page table, a per-process address-space map (VMAs), and a demand-paging fault handler that backs heap, stack, and anonymous `mmap` memory lazily. Together they let a real libc allocator run: a musl program can `malloc`, `qsort`, grow its stack, and fault cleanly, while a wild pointer kills only the offending process and leaves the kernel running.

## Pieces

| File         | Responsibility                                                                                                     |
|--------------|--------------------------------------------------------------------------------------------------------------------|
| `frame.*`    | Size the physical frame pool from the multiboot memory map, capped to the direct-mapped window                     |
| `paging.*`   | 32-bit page tables, the identity map, user-page alloc/free, and the page-fault dispatcher                          |
| `vm.*`       | The VMA model: a small fixed array of `{start, end, prot, type}` regions per process, with find/insert/remove/grow |
| `heap.*`     | The kernel's own bump and region allocator (`mem_reserve`) for kernel structures                                   |
| `usercopy.*` | The `copy_from_user`, `copy_to_user`, and `strncpy_from_user` boundary, fault-aware                                |

## Address-space model

Every process carries a `vm_space_t` (in its PCB) describing its user memory as a handful of virtual memory areas. Each VMA is a half-open, page-aligned range `[start, end)` with a protection (`VM_PROT_*`) and a kind (`VM_CODE`, `VM_DATA`, `VM_HEAP`, `VM_STACK`, `VM_ANON`). This is the structure real kernels build on (Linux `vm_area_struct`, BSD `vm_map_entry`); here it is a 16-entry array, since a teaching process has only a few regions. `vm.c` is pure logic with no kernel globals, so the host seam test (`tests/host/vm_hosttest.c`) exercises find, insert, remove, overlap, and stack growth directly under ASan and UBSan.

`exec` builds the map (`elf_setup_vmas`): one VMA per loaded ELF segment with the segment's real permissions, an empty heap just past the image (`brk == image_end`), and a one-page stack at the top growable down to `stack_limit`. `fork` copies the parent's map verbatim.

## Eager versus lazy

ELF code and data segments are loaded eagerly at `exec` (their pages are mapped up front), which keeps the loader simple and the boot path unchanged. Everything that grows at runtime is demand-paged: heap pages (`brk`), the stack below its first page, and anonymous `mmap` regions. The 32-bit x86 page tables have no NX bit, so "code" is present, user, read-only and "data", "heap", and "stack" are present, user, writable; only the write bit distinguishes them, which is what copy-on-write keys on.

## Demand paging

`_page_fault_handler` (vector 14) reads `CR2` and the error code, then handles three cases.

* A fault on a user-space address of a live process goes to `vm_resolve`. If a VMA covers it (or it is a legal one-page-or-more stack growth below the stack VMA and at or above `stack_limit`), a fresh frame is allocated, zero-filled, and mapped; a write to a read-only region or an address in no VMA fails. This keys on the faulting address being in user space, not on the CPU mode, so a kernel touch of a lazy user page resolves here too.
* An unrecoverable fault taken in user mode segfaults the process. `terminate_current_process` (in `kernel.c`, shared with the CTRL+C path) tears it down and schedules the parent to collect the zombie; if the dier was a real init it panics instead (a rescue/no-init debug shell regains control). The machine survives.
* A fault in kernel mode with no valid user cause is a genuine kernel bug, and panics loudly.

Newly demand-paged pages are always zeroed, so heap, stack, and `mmap` memory reads as zero and a recycled frame never leaks a previous process's data.

## Syscalls

* `brk` (45): the heap is a single demand-paged `VM_HEAP` VMA `[heap_start, page_up(brk))`. Growing extends the region (pages fault in lazily); shrinking frees the pages above the new break. Returns the resulting break, matching musl's `__brk` contract.
* `mmap2` (192): anonymous private mappings only (what musl's allocator uses), placed by bumping `mmap_top` down from a fixed base between heap and stack. Pages are demand-paged and zero-filled; the file-backed path returns `-ENOSYS`.
* `munmap` (91): drops the VMA bookkeeping (trimming or splitting partial overlaps) and frees the backing frames.

## Fault-aware usercopy

Because user memory is now demand-paged, the kernel must not dereference a not-yet-present user page directly. `copy_from_user`, `copy_to_user`, and `strncpy_from_user` range-check the pointer and then pre-fault each page through the same `vm_resolve`: a valid-but-lazy page is backed (with write permission for copies into user space), and a page outside every VMA returns `-EFAULT` instead of faulting in kernel mode. `strncpy_from_user` faults one page at a time as it crosses boundaries, since the string may end before a later, legitimately unmapped page.

## Testing

* `tests/vmprobe`: brk grow and shrink, heap read and write, anonymous `mmap` map/read/write/`munmap`, deep recursion to force stack growth, and the lazy-pages-read-zero check. Prints `VMPROBE OK`.
* `tests/vmfault`: writes through a wild pointer into an unmapped gap; the process must die with the kernel surviving and the shell returning.
* `tests/musl/mallocqsort`: a musl program that `malloc`s, `qsort`s, and makes a 256 KB (mmap-backed) allocation. Prints `MALLOC QSORT OK`.
* `tests/host/vm_hosttest` and `tests/host/frame_hosttest`: native ASan and UBSan seam tests for the pure VMA and frame-sizing logic (`make verify-host`).
