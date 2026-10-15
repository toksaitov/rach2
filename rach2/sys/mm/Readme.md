# Memory

`rach2/sys/mm/` is the kernel's memory manager. It counts the physical memory the loader's map offers and gives it out a frame at a time; it lays out the kernel's half of the address space, with a heap that allocates and frees objects of any size; it gives each process an address space of page tables and regions, backs a page when it is first touched, and shares a parent's pages with a forked child until one of them writes; it copies data between the kernel and user memory without faulting on a bad pointer; and it keeps pages of files and of a KRFFS disk root in free frames, which it gives back when memory runs short. Pages are 4 KiB. Each architecture's `mmu.c` builds and walks its own page tables, and the code here decides what goes in an entry. There is no swap.

## Interfaces

### System calls

The calls that change a process's regions are in `sys/syscall_proc.c`. [brk(2)](../../../man/man2/brk.2) gives their contracts, and the others are aliases of that page.

| Call | i686 | x86-64 |
| --- | --- | --- |
| `brk` | 45 | 12 |
| `mmap` | none | 9 |
| `mmap2` | 192 | none |
| `munmap` | 91 | 11 |
| `mprotect` | 125 | 10 |
| `mremap` | 163 | 25 |
| `msync` | 144 | 26 |
| `mincore` | 218 | 27 |
| `madvise` | 219 | 28 |
| `mlock` | 150 | 149 |
| `munlock` | 151 | 150 |
| `mlockall` | 152 | 151 |
| `munlockall` | 153 | 152 |
| `mlock2` | 376 | 325 |

* i686 has `mmap2`, whose offset is in pages, and not i386's older `mmap`, 90; x86-64's `mmap` takes the offset in bytes. Both reach `ksys_mmap_pgoff`.
* [fork(2)](../../../man/man2/fork.2) and [execve(2)](../../../man/man2/execve.2) make and replace address spaces, [ptrace(2)](../../../man/man2/ptrace.2) and `/proc/PID/mem` reach another process's pages, `shmat` and `shmdt` map System V segments ([the IPC Readme](../ipc/Readme.md)), and [prctl(2)](../../../man/man2/prctl.2) sets the dumpable flag an address space keeps.
* `/proc/meminfo`, `/proc/slabinfo` and `/proc/PID/maps` report what this code counts ([proc(5)](../../../man/man5/proc.5)); `/dev/mem` maps device memory ([mem(4)](../../../man/man4/mem.4)); `/dev/framefail` fails a frame allocation on purpose ([bootlog(4)](../../../man/man4/bootlog.4)). [memory(7)](../../../man/man7/memory.7) gives the boot's memory report as a user reads it, and [cmdline.md](../../cmdline.md) the boot-line switches `rach2.framefail`, `rach2.framehigh`, `rach2.fswindow` and `rach2.fsdisk`.

### The kernel's layout

Each architecture's `cpu.h` defines the regions:

| Region | i686 | x86-64 |
| --- | --- | --- |
| User space | `0x02000000` to `0xC0000000` | `0x10000` to `0x7FFFFFFFF000` |
| Physical memory | the first 32 MiB, at their own addresses | all of it within the reach, at `0xFFFF800000000000` plus its address |
| Heap | `0xC0000000`, 256 MiB | `0xFFFFC00000000000`, a region of 512 GiB |
| Page-cache window | `0xD0000000`, at most 512 MiB | `0xFFFFC20000000000`, 512 MiB |
| MMIO | `0xF0000000`, 128 MiB | `0xFFFFC08000000000`, 128 MiB |
| Temporary mappings | `0xF8000000`, 1024 pages | none |
| Boot modules | `0xF8400000`, 64 MiB | `0xFFFFC18000000000`, 64 MiB |
| Kernel image | its physical address, from 1 MiB | `0xFFFFFFFF80000000` plus its physical address |

Other values that differ:

| | i686 | x86-64 |
| --- | --- | --- |
| Reach, the first physical address the kernel cannot address (`mmu_phys_reach`) | 4 GiB | 64 TiB, or 64 GiB without 1 GiB pages |
| `HARD_MEMORY_LIMIT`, the top of the DMA pool | 32 MiB | 32 MiB |
| The frame table | at the boot allocator's break | at the top of the highest usable range above 32 MiB that has room |
| Page tables made after the boot | heap pages | frames, through the direct map |
| `kmap_temp` | a slot of the temporary mappings | the frame's address in the direct map |
| `ELF_ET_DYN_BASE`, a position-independent program's base | `0x08000000` | `0x0000555555554000` |
| `USER_MMAP_BASE`, below which mappings go | `0xB0000000` | `0x00007F0000000000` |
| `USER_STACK_BASE`, the stack's top page | `0xBFFFF000` | `0x00007FFFFFFFE000` |

### Kernel functions

* `paging.h`: frames (`alloc_frame`, `alloc_user_frame`, `alloc_kernel_frame` for the DMA pool, `frame_ref`, `frame_unref`, `frame_free_count`), kernel mappings (`map_mmio`, `map_kernel_frame`, `premap_kernel_window`, `kmap_temp`), and user pages (`map_user_frame`, `unmap_user_range`, `paging_protect_range`, `paging_fork_copy`, `vm_prefault_range`, `paging_access`). [The architectures' Readme](../arch/Readme.md) lists what each `mmu.c` defines under this header.
* `kheap.h`: `kmalloc`, `kzalloc` and `kfree`; `kalloc_pages`, `kalloc_cache_page` and `kfree_pages` for whole pages; `kalloc_guarded` and `kfree_guarded`.
* `usercopy.h`: `copy_from_user`, `copy_to_user`, `strncpy_from_user`, `strnlen_user`, `get_user_u32` and `cmpxchg_user_u32`, through which a system call reaches a pointer marked `__user` (`sys/lib/kuser.h`).
* `mm.h` and `vm.h`: the address space, `struct mm`, and its regions, `vm_space_t`.
* `filemap.h` and `shmem.h`: the files `mmap` maps, the calls a file's descriptors make so that the file and its shared mappings agree, and a file's bytes kept in frames.
* `pressure.h`: `mem_balance`, `mem_reclaim`, `mem_may_take`, and the interrupt depth.
* `pagecache.h` and `filecache.h`: the two caches. `frame.h`, `heap.h` and `modlayout.h`: the boot's part.

The file system provides what `mm.c` and `filemap.c` call back: `fs_exe_open`, `fs_exe_lock`, `fs_exe_close`, `fs_read_at` and `fs_write_at`, in `sys/syscall_fs.c`.

## Design

A file named alone is in `sys/mm/`; other paths start at `rach2/`.

### The boot

`kernel_main` (`kernel.c`) sets memory up in this order.

1. `modlayout_check` refuses a module that is empty, ends past 4 GiB, starts below the end of the kernel image or overlaps another, with the module's number and range in the panic. A module that ends within the first 32 MiB moves the boot allocator's break past it, and `init_mm` (`heap.c`) moves the break to `_end`, the end of the image, if it is lower.
2. `frame_map_build` (`frame.c`) reads the loader's memory map, in 64-bit arithmetic, into a sorted list of disjoint ranges: those the map calls available, less every range of another type that overlaps them, with what lies at or above the reach counted apart as unreachable. Without a usable map it takes `mem_upper` as one range from 1 MiB. The list has room for `FRAME_MAP_RANGES`, 265: the boot record's 256 entries give at most 256 ranges, and each later reservation, one of at most eight modules or the frame table, splits at most one more. The kernel then reserves the range from 0 to `_end` and each module.
3. `init_paging` (`paging.c`) makes the frame table, a 32-bit word for each frame below the top of the map, which the architecture places (`mmu_frame_table`). On i686 it is carved at the break, where the kernel reaches it before paging is on. On x86-64 the boot first builds and loads the direct map, then puts the table at the top of the highest usable range above 32 MiB that has room, so the DMA pool keeps its room, and at the break when no range has. A table on memory the map does not offer refuses the machine with `memory: refused: the frame table needs memory past what the map offers after the kernel's reservations`.
4. `mmu_init` builds the kernel's directory, `mmu_start` turns translation on, and `kheap_init` starts the heap.
5. `guard_boot_stack` (`kernel.c`) unmaps the image's page below the boot stack, and `map_modules` maps each module into the module region from wherever the loader put it.
6. The kernel prints `memory: usable N KiB, unreachable N KiB, reserved N KiB, managed N KiB`. Usable and unreachable are the available bytes below the reach and at or above it, reserved is what the boot took of the usable bytes, and managed is the count of free frames in the table above the break. A machine with less than `FRAME_MIN_MANAGED_KIB`, 1024 KiB, managed is refused with `memory: refused: N KiB managed after the kernel's reservations, at least 1024 KiB needed`, and stops.
7. `init_pagecache` makes the page-cache window's page tables before the first process copies the kernel's directory.

`mem_reserve` (`heap.c`) is the boot allocator: it moves the break up, never frees, and returns 0 for a reservation that would pass `HARD_MEMORY_LIMIT`. Once the frame table exists it passes over frames in use and marks the frames it takes.

### The frame allocator

The frame table (`_frames` in `paging.c`) has, for each frame, 0 when it is free, a reference count below `FRAME_REF_MAX` when it is in use, and `FRAME_ABSENT` for a frame the map does not offer. Every writer keeps the free counts, so `frame_free_count` reads one word.

The frames below `HARD_MEMORY_LIMIT` are the DMA pool. They lie below 4 GiB on both architectures, so a device takes the physical address of one in a 32-bit register. `alloc_kernel_frame` scans the pool from its top down and returns the frame's kernel address; the page cache, `sys/block/request.c`, `ahci.c` and `nvme.c`, `sys/usb/core.c`, `hub.c` and `xhci.c`, and `sys/network/driver/e1000.c` take frames from it.

`alloc_frame`, for the heap's pages, x86-64's page tables and the stack pages exec writes the arguments into (`sys/proc/uimage.c`), and `alloc_user_frame`, for user pages, scan above the pool first, then the pool from its bottom up while more than 64 of its frames (`WINDOW_KEEP`) are free, so that `alloc_kernel_frame` finds frames at its top. A hint for each part says that no frame below it is free, so a scan does not start from the bottom each time. Under `rach2.framehigh`, on a machine with memory above 4 GiB, the scan starts at 4 GiB.

On i686 nothing above the pool is reached by its physical address. A user frame is zeroed, copied and filled through one of the 1024 temporary mappings (`kmap_temp`, `paging_zero_frame`, `paging_copy_frame`), and a caller may sleep while it has one. A released slot stays mapped until no slot is free; then every released slot is unmapped at once and every processor drops its translations, once for up to 1024 releases, as Linux's `kmap` does with its pool. On x86-64 `kmap_temp` returns the frame's address in the direct map.

`alloc_frame` and `alloc_user_frame` ask `mem_may_take` (`pressure.c`) first. It refuses a frame to a process that is not privileged (`proc_privileged`, `sys/proc/process.h`) once the free count is at or below the reserve, a thirty-second of managed memory, which is kept for root's login and for the writes reclaim starts. An interrupt handler, the kernel outside any process and a process that is reclaiming take from the reserve. A refused user frame, or one that cannot be had at all, is logged with the process's pid and sets `frame_refused` in its record, which the fault handler reads. Under `rach2.framefail`, a count written to `/dev/framefail` makes the count-th frame allocation inside a system call fail.

### Page tables

The shared code reaches the tables through `mmu_user_table`, a directory's leaf table for a user address, which it makes on request; `mmu_user_skip`, the next user address whose leaf table a directory may have; and `mmu_kernel_table` (`paging.h`). On i686 a directory has two levels of 1024 entries, and on x86-64 four levels of 512; [the architectures' Readme](../arch/Readme.md) describes each architecture's tables and the kernel's directory. `clone_page_directory` copies the kernel's entries into a process's directory, so every table of the kernel's layout on i686, and every PML4 entry of it on x86-64, must exist before the first process starts; a kernel mapping made later is then seen by every process. On i686 the boot makes the 256 tables of the gigabyte from `0xC0000000`. On x86-64 it makes the eight PML4 entries, 4 TiB, of the heap, the MMIO region, the modules and the page-cache window, and the tables of the MMIO and module regions, and `premap_kernel_window` makes those of the heap's part and of the window. The x86-64 direct map reaches from 0 to the top of usable memory, at least 4 GiB, in 1 GiB pages where the processor has them and otherwise in 2 MiB pages, whose directories are carved below 32 MiB.

Before the heap exists the boot carves tables at the break; on x86-64 one that would fall past the memory the map offers refuses the machine with `memory: refused: the kernel's page tables need memory past what the map offers after the kernel's reservations`. After `mmu_heap_ready` a table is a heap page on i686, whose physical address the kernel's directory gives (`paging_virt_to_phys`), and a frame from `alloc_frame` on x86-64.

An entry has the processor's bits and two marks of the kernel's own in bits the processor ignores: `PTE_COW`, bit 9, a page shared copy-on-write, and `PTE_NONE`, bit 10, a resident page of a region with no access, present but supervisor-only and read-only. `PTE_NOEXEC` is the no-execute bit, 0 on i686, which has none. Copy-on-write and the page cache's dirty marks depend on `CR0.WP`, which makes a write from ring 0 to a read-only page fault.

The walks over a user range step from one leaf table's span to the next and pass an absent upper level in one step (`user_span`), so a region of terabytes with few pages costs few steps.

`paging_flush_dir` drops a translation. For a user address it flushes this processor's when the directory is loaded and has every other processor that translates through the directory flush it, waiting for each (`smp_tlb_shootdown`, `sys/proc/smp.c`), so the caller may free the frame next. A processor whose running thread has no address space is not interrupted: the directory is marked stale there, and the next switch to one of its threads loads it again. For a kernel address every other processor drops all its translations before it next takes the big lock.

`map_mmio` maps a device's registers uncached into the MMIO region, for the life of the kernel, and returns the address the driver uses; `map_mmio_typed` takes other cache bits, `PTE_WC` for a framebuffer.

### The kernel heap

`kheap.c` is the heap. `kheap_size_for` gives it an eighth of managed memory, rounded down to 4 MiB, at least 256 MiB and at most the region: 256 MiB on i686 whatever the machine has, and on x86-64 256 MiB up to 2 GiB managed and the whole 512 GiB from 4 TiB. A page is mapped to a frame from `alloc_frame` when it is allocated and unmapped when it is freed. The map of the region's pages, a bit each, is in the image for a region of 256 MiB and in the region's first pages for a larger one, a frame for each 128 MiB.

* An object of at most 2048 bytes is a block of one of eight classes, 16 to 2048 bytes by doubling, carved from a page with a free list for each class; a larger object takes whole pages. A class page and the first page of a large object begin with a 16-byte header that gives the class or the page count, which `kfree` finds at the base of the pointer's page. `kheap_class` gives each class's counts for `/proc/slabinfo`.
* `kalloc_pages` gives the lowest run of pages that fits, with no header. `kalloc_cache_page`, the buffer cache's, takes pages from the top of the region down, as Linux keeps reclaimable pages apart, so the cache's holes lie among its own pages and not among the runs the rest needs.
* The top 16 MiB is a reserve of sixteen runs of 256 pages, which a request of 64 pages or more takes when the rest of the region cannot meet it, so a request of a megabyte is met beside a full cache. It costs no frame until a run is taken.
* `kalloc_guarded` gives fewer than 64 pages with the page below them marked taken and never mapped, a guard that takes no frame. A kernel stack comes this way (`kstack_alloc`, `sys/syscall_proc.c`); [the architectures' Readme](../arch/Readme.md) says how a fault in the guard ends.
* The heap never sleeps. A request it cannot meet returns NULL, and the caller answers ENOMEM or the error its contract gives.

The heap's spinlock covers its page map, runs, free lists and counts, and the frame allocator's covers the frame table and its counts and hints. Each is taken with interrupts off after the big lock, the heap's before the frame allocator's. The regions, the page tables and the caches change with the big lock held, which every entry to the kernel takes ([the kernel's document](../../Readme.md)).

### Address spaces

`struct mm` (`mm.h`) is an address space: a page directory, its regions, and what `/proc` reports of the image. It has two counts, as Linux's `mm_struct` has `mm_users` and `mm_count`. `users` counts each thread that runs in it, an `execve` that builds it, and a call from another process that reads or writes its pages, such as ptrace's, for the length of the call; the last user's `mm_put` frees the pages, the page tables, the directory and the regions. `holds` counts the users as one and each `/proc/PID/mem` descriptor, and the last hold frees the record. Such a descriptor takes a user's count only while a user remains (`mm_get_live`), so it keeps none of the memory of a process that has ended or executed another program.

`fork` gives the child a copy, made by `mm_new`, `paging_fork_copy` and `vm_copy`; `clone` with `CLONE_VM`, which `vfork` uses, gives it the parent's own with one more user. `execve` builds a new address space while the old one runs and drops the old one when it commits. A thread's exit drops its own (`mm_drop`), loading the kernel's directory first when the space it frees is loaded, and the last put makes every other processor that still has the directory loaded switch to the kernel's (`smp_space_release`).

The address space keeps the program's file, as Linux's `mm->exe_file`: a description of it that opens nothing (`fs_exe_open`), which `/proc/PID/exe` reaches and fork's copy shares. Every address space that keeps a file is on a list `mm_exe_holds` reads, and the KRFFS root counts each among the holders of the file's node, so an unlink of a running program, or a rename over it, leaves its node, without a name, until the last address space that keeps it goes. `mm_exe_close` takes the lock the release needs (`fs_exe_lock`) while the address space is still on the list, so no count of the file's holders runs between its leaving and the release.

The dumpable flag of `prctl` is kept here, one for every thread that shares the address space. It is set in a new address space, cleared by an exec of an image that runs with ids its caller lacks or that its caller may not read, copied by fork, and cleared when a sharer's effective uid or gid changes.

### Regions

`vm.c` keeps a process's regions in an array from the heap, sorted by address, so a lookup is a binary search and a free range is a gap between two entries. The array starts with 16 entries and doubles up to `VMA_LIMIT`, 65536, the bound Linux keeps as `max_map_count`; a change that needs one more is refused, and the kernel logs `vm: OPERATION: pid N (NAME) has used all 65536 of its mappings` once for the process. `vm_reserve` makes room for the entries a change needs before the change starts, so a split either fails with nothing changed or cannot fail.

A region, `vma_t`, is a half-open, page-aligned range of a type: `VM_CODE` or `VM_DATA` for an ELF segment, `VM_HEAP` for the heap `brk` grows, `VM_STACK` for the main stack, `VM_ANON` for private anonymous memory, `VM_FILE` for a private mapping of a file, and `VM_SHARED` for shared anonymous memory or a shared mapping of a file on tmpfs or ext2. Its protection is `VM_PROT_READ`, `VM_PROT_WRITE` and `VM_PROT_EXEC`, with two marks beside them that splits, merges and moves keep: `VM_PROT_NOWRITE`, a region `mprotect` may never make writable, and `VM_PROT_IO`, `/dev/mem`'s frames, which no fault backs and neither a core file nor `/proc/PID/mem` reads, since a read of a device register can change the device. Neighbours of the same type and protection merge, and two that map a file merge only where its pages continue.

A region that maps a file has a `struct vm_file`, with a reference for each region and each fault reading it, and the file's page at the region's start. The last reference puts the file on a list that `vm_file_reap` empties once the regions and the page tables agree again, at the end of `munmap`, `mmap`, `mremap`, `brk` and the other changes. A release may sleep while it closes the file, and a region still in the array then would be copied by a fork, or reached by a fault, after the file had gone.

### A new image

`execve` (`exec_build`, `sys/syscall_proc.c`) builds the new image in an address space of its own, so a failed load returns to its caller. `elf_load_image_at` (`sys/proc/elf.c`) loads each `PT_LOAD` segment whole into zeroed frames, then makes read-only every page no writable segment covers and no-execute every page no executable one covers. A position-independent program goes at `ELF_ET_DYN_BASE` and a position-independent interpreter just below `USER_MMAP_BASE`. Each segment gets a region, the heap starts empty at the program's end, and the stack's region ends at the top of user space and covers the pages from `USER_STACK_BASE` down that the arguments and the environment took. The stack may grow until the whole stack is the soft `RLIMIT_STACK` at the exec, never below `USER_MMAP_BASE` (`exec_stack_window`), and to at most 8 MiB for an image that runs with ids its caller lacks. A whole page of a file comes through the file page cache (`exec_file_read`).

### Page faults

`_page_fault_handler` (`paging.c`) tries, in this order:

1. A fault at a user address while the current process has an address space, from user mode or from the kernel touching a page not yet backed, goes to `vm_resolve`, unless it is an instruction fetch from a page of no executable region (`fetch_refused`), which only x86-64's error code reports.
2. A kernel fault inside the page-cache window goes to `pc_fault`, and panics when the cache cannot serve it.
3. A user-mode fault that failed because the frame allocator refused a frame ends the process with `SIGKILL`, as Linux's OOM killer does, since a `SIGSEGV` handler that returned would fault again.
4. Any other user-mode fault, one in the page-cache window included, sends `SIGSEGV`, with `SEGV_ACCERR` for an address in a region and `SEGV_MAPERR` otherwise.
5. A kernel fault panics, with `page fault: a kernel stack ran out` when it is in the guard page below the running thread's kernel stack.

`vm_resolve_once` backs one page. Outside every region the one legal access grows the stack: `vm_grow_stack` extends the stack's region down to the page when the page is at or above `stack_limit`, the growth fits `RLIMIT_AS`, and no other region lies between. Inside a region the protection must allow the access. Then a present page that allows it needs nothing; a write to a present page that is not writable goes to `cow_fault`; a page of a `VM_PROT_IO` region is never backed; a page of a private mapping of a file is read into a new frame (`file_fault`); a page of a shared region is the frame the file or the shared memory has for it (`shared_fault`); and any other page gets a zeroed frame. A file's read may sleep, so the file is held across it and the region looked up again after, and when the region changed or the page was backed meanwhile the resolution runs again.

A new frame is filled or zeroed before it is mapped, since a thread on another processor runs in user mode without the big lock and would see the page at once. The entry is writable when the region is and no-execute when it has no `VM_PROT_EXEC`. A resolution that may need a frame first lets the caches give frames back (`mem_balance`), which may sleep, and looks at the region only after that.

### Copy-on-write

`paging_fork_copy` walks the parent's regions and the leaf tables it has in each, and maps each present user page into the child's directory with the same frame and one more reference. A page of a `VM_SHARED` region keeps its flags in both; a writable page becomes read-only and `PTE_COW` in both; a read-only page keeps its flags. On the first write `cow_fault` makes the page writable in place when this address space has the frame's last reference, and otherwise copies it. A region `mprotect` made read-only keeps its copy-on-write pages read-only, since the region's protection is checked first.

`paging_protect_range` gives the resident pages of a range `mprotect`'s protection: no access makes a page `PTE_NONE`; read-only clears the write bit; write sets it on a page of a shared region or one this address space alone has, and makes any other page copy-on-write.

`paging_access`, for ptrace and `/proc/PID/mem`, reaches another address space's pages through temporary mappings, bringing in a missing page as a fault would. A write to a private page another address space shares gives this one a copy with the same rights (`page_private`), so a breakpoint written into text leaves the text read-only, as Linux's `FOLL_FORCE` does.

### The mapping calls

`ksys_mmap_pgoff` (`sys/syscall_proc.c`) finds what is mapped first, in `mmap_backing`, which may sleep finding a file's path, and the place after, in `mmap_place`, which does not, so no thread that shares the address space can map the range between the check and the insertion. What is mapped is private or shared anonymous memory, a private mapping of a file (`filemap_new`), a shared mapping of a file on tmpfs (`filemap_shared`) or on ext2 (`filemap_disk_shared`), or `/dev/mem`'s frames (`mmap_devmem`). A file on a loop device or a USB stick is refused (`mmap_fault_readable`), since a fault reads it with interrupts off and outside any call, where the block layer can poll only some disks ([the block layer](../block/Readme.md)). A mapping without `MAP_FIXED` or a free hint takes the highest free range below `USER_MMAP_BASE` and above the heap (`vm_find_free`).

`brk`, `munmap` and `mremap` change the regions first and the page tables after, and `madvise` drops pages and leaves the regions; a page never touched has no frame to drop. `mremap` makes every table the new range needs before it moves the entries (`paging_move_range`). `mprotect` changes the regions before the resident pages, so a fault in between reads the protection being set. `mlock` and the calls like it check the range and change nothing.

### Files that regions map

`filemap.c` makes the `struct vm_file` of a mapping, a `struct filemap` with a copy of the descriptor, which keeps the open file description, so a program may close the descriptor after `mmap`. On the KRFFS root a mapping counts among the holders of the file's node (`filemap_holds`), as a descriptor does.

`shmem.c` keeps a file's bytes in memory, a frame for each page, made when the page is first written or mapped: a tmpfs file's ([tmpfs](../fs/tmpfs.Readme.md)) and shared anonymous memory's. The table has one reference to each frame and each mapping of a page one more. A truncation first takes the pages it frees out of every region that maps them (`filemap_shmem_cut`), so a later touch maps the page the file has then.

A file on ext2 that shared mappings map has a `struct dshare` with its pages in a `struct shmem`, found by mount and inode number, which on ext2 identify one file while anything has it open. A page is read from the file at its first touch. The calls on the file's descriptors keep the file and the pages in step (`filemap_file_reading`, `filemap_file_written`, `filemap_file_truncated`, `filemap_sync_file`), and [brk(2)](../../../man/man2/brk.2) says when the pages go back. A page goes back whole up to the file's size, since the kernel does not read the processor's dirty bits, with the mount's namespace lock held from before the size is read to the end of the write, so a truncation comes wholly before or after it.

### Checked user access

`copy_from_user` and `copy_to_user` (`usercopy.c`) check the range against the user range without overflow, then bring every page in with `vm_prefault_range` (`paging.c`), which resolves each page as a fault would and answers 0 for a page in no region or in one that does not allow the access; the call then returns EFAULT, and the kernel never faults on a bad user pointer. A resolution may sleep while another thread unmaps or protects a page already brought in, so once every page is in the range is checked again with nothing that sleeps, and brought in again when a page went. `strncpy_from_user` and `strnlen_user` bring pages in one at a time, since a string may end before a page that is legitimately unmapped. `get_user_u32` and `cmpxchg_user_u32`, which `sys/proc/futex.c` uses, make one load or one compare-and-exchange of an aligned word.

### Memory pressure

The buffer cache and the disk page cache have a target of a quarter of managed memory together, half each ([the VFS's Readme](../fs/vfs.Readme.md) gives the buffer cache's limit). `mem_balance` (`pressure.c`) runs `mem_reclaim` when the free count is below a sixteenth of managed memory, and reclaim asks the buffer cache, the file page cache and the disk page cache in turn until the free count reaches an eighth or nothing more comes back. A cache gives clean entries first and writes dirty ones back only from a system call with interrupts on, so a page fault takes clean frames alone. A reclaiming process takes from the reserve, which the writes it starts need.

`mem_balance` runs before the allocation that needs the room: in `fork`, `execve`, `brk`, `mmap`, `shmat`, a fault's resolution and a write through `paging_access`. The allocators themselves never sleep. `mmap` and `brk` reserve addresses only, so a fault after them may find no frame and end the process with `SIGKILL`.

### The file page cache

Exec reads a program a page at a time and a private mapping reads a page at its first touch, so a program started again would read its pages from the disk again, and the buffer cache, at most half the heap, does not keep a program of hundreds of megabytes. `filecache.c` keeps a copy of each whole page either path reads from a regular file on ext2, in a frame of its own, found by mount, inode number and generation. A copy is used only while the file has the size and the modification time it had when the page was read. A write, a truncation, an allocation and a change of size through the VFS drop the file's pages, and an unmount every page of its files; each counts as a change, and a page read while a change ran is not kept. A mapping gets a copy, so nothing a process stores reaches the cache. The cache takes a frame only while a quarter of managed memory is free, and gives frames back, the least recently used first.

### The disk page cache

`pagecache.c` maps a KRFFS root into the kernel: a window at `KVA_PCWIN`, supervisor-only, over a backing store read and written a page at a time. KRFFS reaches the window through pointers ([krffs.Readme.md](../fs/krffs.Readme.md)), and a fault in it is the cache's miss. The backing is the RAM-disk module under `rach2.fswindow`, or the KRFFS partition of the boot disk under `rach2.fsdisk`, `rach2.root=disk` or a root chosen by its identity. The disk must be `BLOCK_FAULT_SAFE` when the root is mounted, as ATA, AHCI and NVMe disks are before they go over to their interrupts ([the block layer](../block/Readme.md)). Any other root leaves the cache without a backing.

On i686 the window runs from `0xD0000000` to the lowest PCI memory BAR whose address lies in the 512 MiB after it, and is 512 MiB when none does. On x86-64 no physical address reaches the window's, so it is always 512 MiB. A root larger than the window is refused.

The cache has `PC_SLOTS`, 2048 slots, and uses as many as half the caches' target allows. A slot takes a frame of the DMA pool when it is first filled, since the backing may give the frame's address to a device, and gives it back under pressure (`pc_shrink`). On a miss `pc_fault` takes a free slot or the clock's victim, written back first when dirty, reads the page, and maps it read-only for a read or writable and dirty for a write; a later write to a page mapped read-only faults again and marks it dirty. The fault path polls the disk with interrupts off and never sleeps.

`pc_writeback` marks the slot `pinned`, which eviction, reclaim and other writebacks respect, then maps the page read-only and clears its dirty mark before the write, so a store during the write marks it dirty again; a failed write leaves it dirty. `pc_sync` returns `PC_SYNC_BUSY` when it passed over a pinned slot and `PC_SYNC_FAILED` when a write failed. `fsync` of a file of the KRFFS root answers EIO on either, and `sync` repeats while a slot is busy, for at most 3000 ticks, 30 seconds at the PC's 100 Hz.

## Limits

* There is no swap.
* The i686 kernel addresses the first 4 GiB of physical memory, and every page it maps is executable. The x86-64 kernel addresses 64 TiB, or 64 GiB without 1 GiB pages.
* The heap is 256 MiB on i686 whatever the machine has.
* A file's pages are numbered in 32 bits: a mapping of a file or of shared memory past page 2^32 is refused with EOVERFLOW, and a file in memory ends at 16 TiB less a page.
* A writable shared mapping of a file is possible on tmpfs and ext2 alone. A store past the end of an ext2 file through a shared mapping is lost, and a touch past the end of a mapped tmpfs file ends in `SIGSEGV`, where Linux sends `SIGBUS` for both.
* A file on a loop device or a USB stick cannot be mapped.
* A process has at most 65536 regions.
* A KRFFS disk root is at most 512 MiB, less on i686 where a BAR shortens the window, and the disk page cache keeps at most 2048 pages.
* The MMIO region never frees a mapping, the modules together have 64 MiB and must lie below 4 GiB, and i686 has 1024 temporary mappings.
* Addresses are not randomized.
* The `mlock` calls change nothing, and `RLIMIT_MEMLOCK` is not kept.

## Tests

`tests/run NAME` runs a suite on the architecture in `ARCH`, i686 by default, and `ARCH=x86_64 tests/run NAME` on x86-64 ([the suite inventory](../../../tests/system/suites.md)). `default`, `fault`, `memory`, `pools`, `ext2`, `disk` and `disk-ahci` are in the core tier, which `make test` runs.

* `tests/run default` runs the guest probes ([the tests' Readme](../../../tests/Readme.md)): `vmprobe` (the heap, anonymous memory and stack growth), `forkro` and `cowprobe` and `cowregrant` (copy-on-write), `forkleak` and `oomprobe` (frames that come back, and the `SIGKILL` of a process out of memory), `usercopyfault` (EFAULT for a path that runs into an unmapped page), `musl/vmamusl` (300 mappings at once), `musl/mapmusl` (mappings of files and of shared memory, `mprotect`, `mremap`, `madvise`, `msync` and `/proc/self/mem`) and `musl/callsmusl` (`mlock` and `mincore`).
* `tests/run fault` runs `vmfault`, `vmro` and `winprobe`, which touch no mapping, a read-only page and the page-cache window, and each must die on `SIGSEGV` with the machine running.
* `tests/run memory` requires the recovery kernel at 8 MiB to print `memory: refused` and not fault, checks the report's four numbers at 64 MiB, 512 MiB, 2 GiB, 3 GiB and 4 GiB against the map and the size, runs `memprobe` over memory past 32 MiB, fails frame allocations through fork and exec under `rach2.framefail`, and runs kernel stacks out.
* `tests/run bigmem`, on x86-64 alone, requires all of a machine's 8 GiB to be counted and a program's 5 GiB of pages to read back as written, and on a processor without 1 GiB pages a machine of 72 GiB to be counted to the direct map's 64 GiB.
* `tests/run pools` requires the heap to come back to where it started after a thousand pipes, ptys and sockets each.
* `tests/run ext2` runs `musl/mapmusl` on an ext2 root, with shared mappings written back to the file.
* `tests/run disk`, `disk-ahci`, `stress` and `stress-ahci` write more than the disk page cache keeps through a KRFFS disk root.
* `tests/run smp` runs fork, exec and `munmap` on several processors at once.

`tests/run host` runs the host tests ([the host tests' Readme](../../../tests/host/Readme.md)): `frame_hosttest.c` (the map, from QEMU's, a laptop's, a UEFI map of 256 entries and synthetic ones, with each architecture's reach), `kheap_hosttest.c` (the heap's size, runs, reserve and guards), `paging_hosttest.c` (`paging.c` with i686's `mmu.c`: the range checks at the top of the address space, the temporary mappings and the guard's panic), `vm_hosttest.c`, `mm_hosttest.c`, `modlayout_hosttest.c`, `pagecache_hosttest.c`, `pagecache_ring_hosttest.c` (the slots against a lookup on a KRFFS disk root) and `tmpfs_hosttest.c` (`shmem.c`). `open_hosttest.c` compiles `filemap.c`, and `pipe_copy_hosttest.c` and `unix_hosttest.c` compile `usercopy.c`. The mutation scripts `frame_mutate.sh`, `kheap_mutate.sh`, `paging_mutate.sh`, `pagecache_mutate.sh`, `pagecache_ring_mutate.sh` and `kstack_mutate.sh` in `tests/host/`, and `tests/system/stackoverrun_mutate.sh` and `ext2_map_mutate.sh`, prove their rows with faults in private copies of the sources, and are run by hand.

`filecache.c` and `pressure.c` have no host test, and no suite runs the debug shell's `isolate` command, which writes one address in two page directories.
