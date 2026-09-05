# Milestone 46: memory

## The goal

The kernel uses the memory the machine has. On a notebook with 2 GiB it uses 2 GiB, on one with 3 GiB it uses 3 GiB, and on one with 4 GiB it uses what the processor can address below 4 GiB, and it says at boot how much it found, how much is reserved and how much it manages. No table of sixteen decides how many processes, files, ptys, pipes, sockets or mounts there can be: each is an object allocated when it is needed and freed when it is done, and the only bounds left are the ones physics, a format or a policy sets, each named as such. Work that cannot proceed for lack of a resource waits where waiting is safe and is refused by name where it is not; nothing is turned into a disk error or dropped in silence. The same kernel binary boots and passes its suite with 64 MiB, 512 MiB, 2 GiB, 3 GiB and 4 GiB in QEMU and on the Lenovo. SMP and PAE are out of scope.

## What the tree settles before this plan

* The kernel identity-maps 32 MiB (`paging.h:16`, `paging.c:160`); `FRAME_DIRECT_MAP_LIMIT` duplicates the constant. `frame_top_available` reduces the Multiboot map to its highest available address (`frame.c:11`), `frame_usable_count` caps it (`frame.c:60`), and `init_paging` marks every frame below that free (`paging.c:145`), holes included; with no usable map the code takes `mem_upper`, and 32 MiB only when that fails too (`frame.c:69`).
* There is no kernel heap that frees. `mem_reserve` is a monotonic allocator over identity-mapped physical addresses that stops at the limit (`heap.c:24`); fresh page tables and directories come from it (`paging.c:55`); page directories and private page tables have free lists of their own.
* Physical and virtual addresses are the same thing to the code: user pages are zeroed and copied through their physical address (`paging.c:375`, `paging.c:460`), the loader writes through it (`elf.c:209`), fork and page freeing compare a virtual address with the physical limit (`syscall.c:5612`, `paging.c:1079`). MMIO is mapped at its physical address (`paging.c:552`), and a kernel page-directory entry added after the first fork is not seen by the processes that exist.
* Boot modules are checked against the 32 MiB limit passed by the caller (`kernel.c:2926`, `modlayout.c:24`), and there are eight module records.
* The page cache keeps at most 1024 resident frames, 4 MiB, reserved for good, and shrinks that on a small machine to a quarter of free frames with a floor of 256 (`pagecache.c:509`); its window is 512 MiB at `0xD0000000`, placed to clear the Lenovo's BARs (`pagecache.h:15`). The buffer cache has 64 buffers (`bcache.h:37`). Frame reference counts are one byte and a saturated count pins the frame for good (`paging.c:853`).
* User addresses run from 32 MiB to 3 GiB (`usercopy.h:22`); programs are linked at `0x08048000`.
* The fixed pools: 16 processes (`process.h:11`), 16 descriptors each (`fd.h:21`) and `16 * MAX_FDS` open-file descriptions (`fd.c:14`), 16 ptys (`pty.h:35`), 16 pipes (`pipe.c:32`), 16 sockets and 16 TCP connections with accept queues of 16 (`socket.h:26`, `tcp.h:26`), 4 KiB TCP rings and one held UDP datagram per socket (`tcp.h:24`, `socket.h:100`), 8 UDP, 8 ARP, 4 raw and 2 packet bindings (`udp.h:14`, `arp.h:22`, `raw.h:38`, `packet.c:23`), 16 block requests each with a page (`request.c:22`), 8 mounts (`vfs.h:46`), 4 ext2 mounts with 64 nodes each (`ext2.h:7`), 4 FAT32 and 4 exFAT mounts with 32 nodes each (`fat32.h:43`, `exfat.h:26`), 2 mounted KRFFS volumes with 16 open records (`krffsmount.c:31`), 4 loop devices (`loop.h:15`), 512 VMAs embedded in each process (`vm.h:43`), synthetic node pools of 16 and 32 (`vfs.c:1201`, `syscall.c:97`), 8 open `/proc` snapshots (`procfs.c:1252`).
* Dependent storage: a wait queue holds `MAX_PROCS` pointers (`wait_queue.h:24`); poll has stack arrays sized by `MAX_FDS` (`poll.c:32`); kernel stacks and exec workspaces are indexed by process (`syscall.c:286`, `:5757`); assembly reads fixed offsets in the process record (`process.h:63`); the `/proc` buffers are sized for one disk, one interface, eight ARP entries and sixteen connections (`procfs.c:86`).
* Interrupts: USB and HDA take fixed MSI vectors (`usb.c:144`, `hda.c:40`) and a vector holds four handlers (`isr.h:116`); the IOAPIC is not programmed.
* Ordinary file offsets are 32 bits while raw and VFS offsets are 64 (`fd.h:96`, `vfs.h:149`); ext2 and exFAT cap a write near 4 GiB (`ext2.c:2053`, `exfat.c:2836`). That is milestone 47's.
* Some bounds are contracts, not capacity: file-system name limits, packet formats, xHCI route strings, `select`'s descriptor set (`poll_select.h:16`), the buffer cache's holding rule and the loop depth (`bcache.h:28`, `block.h:73`). They stay, with their reason written.
* Milestone 45 made a full request pool wait rather than answer `EIO`.
* PAE would expose physical memory above 4 GiB through a different paging format. It would not enlarge a process's virtual address space, and every DMA and mapping path would change with it. Firmware on a 4 GiB machine puts part of the RAM above 4 GiB to make room for PCI apertures, so "4 GiB installed" is less than 4 GiB usable to this kernel, and the boot report says both numbers.

## Decisions

### Physical memory is accounted from the map, all of it

Boot starts with every page unavailable, adds the usable ranges the Multiboot map declares, subtracts the kernel, the modules, the boot data and the allocator's own tables, resolves an overlap conservatively and does its arithmetic in 64 bits so a range ending at 4 GiB is right. Frames are named by number. The frame table, a state and a reference count wide enough that it cannot saturate, is sized from that result. The boot line reports what the map establishes and nothing it does not: usable is the available ranges below 4 GiB; unreachable is the available ranges at or above 4 GiB, which this kernel cannot address; reserved is the kernel, the modules, the boot data and the allocator's tables; managed is usable less reserved, and is what the allocator hands out. There is no installed figure, because the map does not give one (`frame_hosttest.c:59` has a reserved range above RAM that a sum would count). No usable map means `mem_upper`, as today.

### The kernel has a virtual layout and a heap that frees

The identity map of low memory stays for the kernel image, the boot data and DMA that needs a physical address. Above user space, in the gigabyte from `0xC0000000`, the kernel gets regions with fixed places: the heap, a set of temporary page mappings, the page-cache window and an MMIO region into which a controller's BAR is mapped wherever it physically is. Every page table for that gigabyte exists at boot and is shared by every page directory, so a mapping made after the first fork is seen by every process. The heap allocates and frees objects of any size, grows by taking frames from anywhere in physical memory and mapping them virtually contiguous, and is what page tables, kernel stacks, process records and every other kernel object come from. DMA buffers ask for the alignment and the addressability their device needs. The user-page paths, zeroing, the copy-on-write copy and the loader, go through the temporary mappings. The old arithmetic that treated a physical address as a pointer is made uncompilable so no path is missed.

### Objects are allocated, not counted out

Processes, open-file descriptions, ptys, pipes, sockets, connections, mounts, file-system nodes, requests, loop devices, `/proc` snapshots and synthetic nodes are allocated from the heap when made and freed when done, kept on lists and found through indexes where lookup needs them. A descriptor table grows; VMAs are a growable ordered structure. A wait queue holds waiter records the sleeping process supplies, so entering a sleep allocates nothing; poll registers once per watched object. The process record's layout stays what the assembly reads; the record itself is allocated. The `/proc` reports iterate rather than fill a fixed buffer. Hardware rings stay finite, sized within what the hardware allows, with software queues above them. A bound that stays is classified in its header as physical, format, policy or invariant, with the reason.

### Caches are targets, not reservations

The page cache and the buffer cache get a combined target of a quarter of managed memory, use more when memory is idle, and give frames back under pressure; dirty buffers stay dirty until written. A forward-progress reserve, a hundred-and-twenty-eighth of managed memory to start, is kept for completion, writeback and an administrator's login, and the pressure test decides the number.

### Waiting and refusing have rules

A request waits for a device or a slot before submission. Start the transfer deadline when the device starts the transfer; time spent waiting in the software queue does not count, so a queue an hour long is not a failed five-second transfer. Cache pressure starts reclaim and writeback; allocation pressure never marks a medium failed. Interrupt and fault contexts never enter the sleeping allocator; they have bounded operations and reserved resources. A non-blocking call answers `EAGAIN`; an interrupted wait answers `EINTR` where its contract allows. Process creation, descriptor allocation and mapping creation try allocation and reclaim, then return `ENOMEM` or the error defined for the exhausted limit; sleeping inside `fork` until someone exits can deadlock a workload, so it is not done. Device removal and a failed transfer still end the I/O they affect. There is no swap: more work than fits in memory waits in a job queue in userland, which is not this milestone's, and the kernel's part is to refuse by name rather than drop.

### A person is limited by policy, and the policy is configurable

`prlimit64` enforces `RLIMIT_NPROC` per real user across the machine and `RLIMIT_NOFILE` and `RLIMIT_AS` per process, soft and hard, inherited across fork and exec, hard raised by root only, counted against the new user after `setuid`. `init` sets the defaults for the accounts the installer makes, and they are numbers in `/etc/rach2/limits`, not in the kernel. An administrator's way in is the console, which needs no socket; over SSH, dropbear's own connection cap bounds its unauthenticated children.

### Interrupt vectors are allocated

Allocate an available interrupt vector for each device; the fixed vector numbers go, and the metal acceptance reads vectors from `/proc/interrupts` by device name rather than by number (`metal_accept.sh:249`). That is what a second controller of any kind needs first, and milestones 47 and 48 depend on it.

## Phases

### Phase 1: the frame map

Ranges from the map, frames by number, the reservations, the four boot numbers, the wide reference count. Proof: a host test over the recorded maps of QEMU and the Lenovo and over synthetic maps with holes, overlapping entries and a range crossing 4 GiB; a boot at 64 MiB, 512 MiB, 2 GiB, 3 GiB and 4 GiB whose usable and unreachable numbers together match what QEMU was given and whose other two follow the equations, with the harness shown to pass the size to every boot it makes.

### Phase 2: the layout and the heap

The kernel regions above `0xC0000000` with their page tables shared; the heap; MMIO mapped into its region; the modules mapped and reserved wherever the loader put them; the old arithmetic uncompilable. Proof: a boot at 4 GiB whose probe fills pages beyond the old limit with a pattern and reads it back, forces copy-on-write, execs, frees and reuses; kernel objects still created after the old low region is full; a mapping made after a fork seen from every process; frame-allocation failure injected by a boot switch at each step of fork, exec, mount and descriptor-table growth, checked for leaked references and for changes to surviving objects at 64 MiB and at 4 GiB.

### Phase 3: the objects

Every pool in the list above made an allocated object; the wait queues and poll reworked; the `/proc` reports iterating; the remaining bounds classified in their headers. Proof: a row per former pool that crosses the old sixteen, or eight, or four, and a row that makes and frees the objects a thousand times and finds the heap where it started; the host tests run with a small heap so increased capacity does not hide leaks or allocation failures.

### Phase 4: the caches and the rules

Targets and reclaim; the reserve; the waiting rules in the request layer and the allocators; a sleeping device in the pressure row, since polled SATA bypasses the pool until milestone 47. Proof: a row that fills memory with dirty cache and finds it written back and the mount still writable; a row that queues requests past every slot and finds each completed with no deadline expired; the mutation that starts the deadline at the queue makes it fail; a row that exhausts memory from one user, finds that user's `fork` refused with `ENOMEM`, and then logs root in fresh on the console and runs a recovery command to completion, since a login needs processes and the SSH supervisor begins with a `fork` (`init.c:368`).

### Phase 5: limits and vectors

`prlimit64` enforced; defaults from `/etc/rach2/limits`; vectors allocated; the acceptance script reading them by name. Proof: a user that forks past its limit is refused while another is not; a process past its file and address-space limits refused by name; a hard limit raised by a user refused and by root allowed; the limits seen inherited across fork and exec; a `setuid` process counted against its new user; a connection flood while root logs in on the console; a device given a vector it did not have before, with the network and USB suites unchanged.

### Phase 6: the close

Twenty sessions in QEMU building at once as the load row that milestone 47 runs on the machine; the functional rows at 512 MiB, 2 GiB and 3 GiB as well as the full set at the two ends. Proof, once and in this order: build from clean; the full set at 64 MiB and at 4 GiB; everything that comes up fixed; the whole milestone's diff to Codex in units of six files, most serious first; every finding fixed; the full set again; then the Lenovo and the SSH capstone. Update each affected Readme in the commit that changes its contract. Codex writes the prose from verified facts. Problems found on the way that this milestone did not set out to fix are already in `dev-log/50-shortcomings/plan.md` with file and line, and none is fixed here unless the milestone cannot close without it. Record the results in `notes.md` and the milestone's Readme. Add the index entry.

## Risks

* The memory changes affect every kernel subsystem. Each phase compiles and runs the rows it touched; the full set at two memory sizes and the machine come once at the close; phase 2 is allowed to be the longest phase this project has had.
* A heap that frees can fragment; the allocator keeps size classes and the thousand-cycle row measures it.
* Write the rules before implementation. Review specifically for lost wakeups and allocation from interrupt context.
* Larger objects make linear scans and interrupt-disabled loops longer; the load row measures the longest delay of the timer, the console, the network and disk completion under load, and the numbers are recorded. 
