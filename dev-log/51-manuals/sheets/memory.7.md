# memory(7)

NAME: memory - how the kernel accounts and hands out memory. Sources: rach2/sys/mm/Readme.md (Physical memory, Page tables and address spaces, Virtual memory areas, Page faults, Copy-on-write fork, Page cache); rach2/Readme.md "Hardware and limits" (the direct physical-memory limit); the boot record `memory: usable N KiB, unreachable N KiB, reserved N KiB, managed N KiB`; `/proc/meminfo`.

* Physical memory from the Multiboot map, in 64-bit arithmetic; the kernel's virtual gigabyte from `0xC0000000` with 256 page tables shared by every directory; a 32 MiB identity-mapped window is the direct limit, and frames above it are given to user pages first, below it only while more than sixty-four remain free (state the rule as the Readme gives it).
* A process's address space: regions (VMAs) for the program, the heap (`brk` grows or shrinks one range; growth reserves addresses only, pages come on first access) and mappings (`mmap2`, `munmap`); a fault on the page just below the stack region, at or above the limit set when the stack was made, grows the stack; any other fault outside a region, or one the kernel cannot serve a frame for, ends the process with SIGSEGV; `RLIMIT_AS` bounds the regions (limits(7)). A process has a fixed number of regions (`VMA_LIMIT`; the record `vm: ... has used all N of its mappings` once per process).
* Fork copies on write: the parent's pages are shared read-only and copied at the first write.
* The page cache: a window of 4096-byte pages over a disk root, filled on demand and evicted under pressure; a reserve of frames kept for root's login so that a machine out of memory can still be entered (the TCP listener drops a SYN when the free frames are inside that reserve).
* The kernel heap: process records, sockets, requests, buffers; `/proc/meminfo` reports the totals; `tests/run memory` checks the accounting; `tests/run pools` opens and closes a pipe, a pty and a socket a thousand times each and requires the heap bytes in use back at their start and the heap pages within two of it.
* The boot line: `memory: usable ... managed ...`.
* SEE ALSO: limits(7), mmap2(2), brk(2) (phase 3), proc(5).
