# Milestone 49: capacity

## The goal

The system uses the memory the machine has, up to what a 32-bit kernel can
address, and its fixed tables no longer decide how many people can use it:
a process, a descriptor, a pty, a pipe, a socket, a connection or a disk
request that is not there is waited for or refused by name, never turned
into a disk error or a silent failure, and a person cannot take the machine
from everyone else. The target is measured, not promised: eight SSH sessions
with two builds running, then twenty sessions mostly idle with a few
building, on the Lenovo, with numbers in `hardware.md`. A hundred students
compiling at once is not a target of this milestone or of this laptop.

## What the tree settles before this plan

* The kernel identity-maps a 32 MiB window (`paging.h:15`, `paging.c:160`);
  user addresses begin right above it at `USER_ADDR_MIN` (`usercopy.h:22`),
  programs are linked at `0x08048000`, MMIO sits near `0xC0500000` on the
  Lenovo and the KRFFS page-cache window begins at `0xD0000000`
  (`pagecache.h:15`). The frame code reduces the Multiboot map to its
  highest available address and caps it (`frame.c:60`); `paging.c:150`
  then marks that whole range free, holes included. Every kernel path that
  touches a frame does so through the identity map: zeroing at
  `paging.c:375`, copy-on-write at `paging.c:460`, the loader at
  `elf.c:209`, and the page tables, kernel stacks, DMA buffers and every
  kernel object.
* The page cache's 512 MiB is an address window; its resident capacity is
  4 MiB (`pagecache.h:24`). Frame reference counts are one byte and a
  saturated count pins the frame for good (`paging.c:871`).
* The tables: `MAX_PROCS` 16 (`process.h:11`), `MAX_FDS` 16 (`fd.h:21`),
  `OFD_POOL_SIZE` is `16 * MAX_FDS` (`fd.c:14`), `PTY_MAX` 16 (`pty.h:35`),
  `MAX_PIPES` 16 (`pipe.c:32`), `MAX_SOCKETS` 16 (`socket.h:26`),
  `MAX_TCP_CONNS` 16 (`tcp.h:26`), `REQ_POOL` 16 (`request.c:22`),
  `VFS_MOUNTS` 8, `BCACHE_BUFFERS` 64, 64 ext2 nodes held per mount
  (`ext2.h:7`), eight UDP bindings, eight ARP entries, 4 KiB TCP buffers.
  A process carries 512 VMA entries and a 16 KiB kernel stack; wait queues
  are arrays sized by `MAX_PROCS`; the process layout is read by assembly.
* Milestone 45 made a full request pool wait rather than answer `EIO`.
* Dropbear forks once per connection, before authentication and as root
  (`svr-main.c:308`), and once per session; a logged-in student is two
  processes before a shell runs anything, and a connection that never
  authenticates is one process root owns.
* `prlimit64` accepts and ignores; milestone 45 documented that and made it
  refuse a pid it cannot act on and a resource it does not know.
* One processor. No threads. Polled SATA transfers hold the processor while
  they wait.
* PAE would give the kernel physical addresses above 4 GiB through a second
  paging format with 64-bit entries; it would give a process nothing, and
  every DMA and mapping path would change with it.

## Decisions

### Physical memory is accounted first

A frame allocator that takes the Multiboot map as a list of usable ranges
and never hands out a hole. Frames are named by number, not by pointer.
This comes before any window grows, because the present allocator would
give out firmware and PCI ranges as RAM once the window did.

### The identity map stays; user frames go high first

The kernel keeps its identity map of the low region and reaches a frame
above it through a small set of temporary kernel mappings, the way a
32-bit kernel has always done it. The map cannot grow in place, since user
addresses begin right above it, and cannot move above user space without
relinking the kernel and every MMIO and cache mapping; neither is worth
it. Only user frames are allocated from high memory in this milestone,
which means three paths change: zeroing a new page, the copy-on-write
copy, and the loader. Page tables, kernel stacks, DMA buffers, the page
cache and every kernel object stay in the low pool. Moving those too is
the second step, taken only if the measurement says the low pool is what
runs out. User address space is unchanged, so no executable changes. The
processor's own limit, four gigabytes of physical address, is the ceiling;
PAE is not in this milestone.

### The tables get one set of larger bounds, and running out is an answer

The tables are not sized from memory at boot; that is machinery ahead of
any measurement. Each gets an explicit compile-time bound chosen for the
eight-session target, in one header so they are chosen together, with the
dependent storage sized off the same numbers in fact: the open-file pool,
the wait queues, the ext2 node pool, the per-process descriptor table that
the assembly reads. Each table has a counter under `/proc` for the times it
was full. A socket, a pipe or a pty that is not there is `ENOMEM` or
`EAGAIN` by name. The one-byte frame reference count is widened before the
sharing this allows can saturate it.

### A person is limited

`prlimit64` enforces three limits with the meaning they have elsewhere:
`RLIMIT_NPROC` counts the processes of one real user across the machine,
`RLIMIT_NOFILE` and `RLIMIT_AS` bound one process. Each has a soft and a
hard value, both inherited across fork and exec, and only root raises a
hard one; a `setuid` to another user counts the process against that user.
`init` sets the defaults for the accounts the installer makes. An
administrator's way in is the console, which needs no socket and no
connection slot, and that is the guarantee this milestone makes. Over SSH
the guarantee is narrower and stated: dropbear's unauthenticated children
are bounded by a connection cap in the server's own configuration, so a
flood of connections cannot take every process, but a user who fills the
socket table can still keep root out until the sockets time out.

### What is measured

The suites gain a load row: N sessions over SSH in QEMU doing a fixed mix.
The number the Lenovo reaches is what `hardware.md` records, and the
Readme's Limits section says that number and no other.

## Phases

### Phase 1: the frame map

Ranges from the Multiboot map, frames by number, holes never allocated,
the kernel, the modules, the boot data and the allocator's own tables
reserved. Proof: a host test over the recorded maps of QEMU and of the
Lenovo and over synthetic maps with holes, overlapping entries and a range
crossing 4 GiB; and a boot that reports the usable total. This phase must
precede phase 2; phases 3 and 4 do not depend on it.

### Phase 2: high memory for user frames

Temporary mappings; the three user-frame paths changed; the identity-map
arithmetic they used made uncompilable so no fourth path is missed.
Proof: a boot with 4 GiB in QEMU whose probe fills pages beyond the low
pool with a pattern and reads it back, forces copy-on-write on them, execs
and frees and reuses them, and exhausts the low pool while high frames
remain and still forks; the whole suite; the phase ends with a Codex
review of the memory changes as one unit.

### Phase 3: the tables

The bounds chosen together; exhaustion answered by name and counted; the
reference count widened. Proof: a row per table that fills it and requires
the named answer and the counter's rise; a row that shares one frame past
255 times and finds it freed afterwards.

### Phase 4: limits per user

`prlimit64` enforced as decided; defaults from `init`; the connection cap.
Proof: a user that forks past its limit is refused while another user is
not; a connection flood from one host while root logs in on the console;
the phase ends with a Codex review of the limits as one unit.

### Phase 5: measured

The load row; the numbers on the Lenovo. Proof: the numbers, in
`hardware.md`, and the Readme's Limits section updated from them.

### Phase 6: the record

Notes, `tests/run all`, the milestone's Readme and the index entry.

## Risks

* This is the kernel's memory design changed underneath everything; each
  phase ends with the whole suite and a boot on the machine, and phase 2 is
  allowed to be the longest phase this project has had.
* Larger bounds hide a bug that sixteen would show; the host tests keep
  running at sixteen through the same header.
* The measured number may be small. It is written down as it is.
