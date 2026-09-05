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

* The kernel manages a 32 MiB direct-mapped window (`paging.h:15`); user
  addresses begin right above it at `USER_ADDR_MIN`; the frame allocator
  sizes itself from the highest available address in the Multiboot map
  (`frame.c:4`) and does not represent the holes between; every kernel path
  that touches a frame does so through the direct map.
* The page cache's window is bounded at 512 MiB (`pagecache.c`); frame
  reference counts are one byte and saturate.
* The tables: `MAX_PROCS` 16 (`process.h:11`), `MAX_FDS` 16 (`fd.h:21`),
  `OFD_POOL_SIZE` is `16 * MAX_FDS` (`fd.c:14`), `PTY_MAX` 16 (`pty.h:35`),
  `MAX_PIPES` 16 (`pipe.c:32`), `MAX_SOCKETS` 16 (`socket.h:26`),
  `MAX_TCP_CONNS` 16 (`tcp.h:26`), `REQ_POOL` 16 (`request.c:22`),
  `VFS_MOUNTS` 8, `BCACHE_BUFFERS` 64, eight UDP bindings, eight ARP
  entries, 4 KiB TCP buffers. A process carries 512 VMA entries and a
  16 KiB kernel stack; wait queues are arrays sized by `MAX_PROCS`; the
  process layout is read by assembly.
* An exhausted request pool answers `EIO`, which the file systems treat as a
  device that failed and take the mount read-only for good.
* Dropbear forks once per connection and once per session; a logged-in
  student is two processes before a shell runs anything.
* `prlimit64` accepts and ignores (milestone 45 documented it).
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

### The direct map stays small; high memory is mapped on demand

The kernel keeps a direct map of the low region and reaches every other
frame through a small set of temporary kernel mappings, the way a 32-bit
kernel has always done it. Every path that touches a frame is audited and
changed: zeroing, copying, copy-on-write, the loader, page tables, modules,
the page cache, DMA buffers. User address space is unchanged, so no
executable changes. The processor's own limit, four gigabytes of physical
address, is the ceiling; PAE is not in this milestone.

### The tables become sized, and running out is an answer

Each table is sized from the memory the machine has at boot, with a floor
that is today's sixteen and a ceiling the kernel's own structures allow;
the per-process descriptor table stays a fixed shape for the assembly that
reads it, larger. A request that finds no slot waits with a deadline rather
than failing as a device; a socket, a pipe or a pty that is not there is
`ENOMEM` or `EAGAIN` by name. `OFD_POOL_SIZE` and every other table that is
sized off another is sized off it in fact.

### A person is limited

`prlimit64` enforces processes, open files and address space per user;
`init` sets defaults for the accounts the installer makes; root keeps a
reserve of processes and requests so an administrator can always log in.

### What is measured

The suites gain a load row: N sessions over SSH in QEMU doing a fixed mix.
The number the Lenovo reaches is what `hardware.md` records, and the
Readme's Limits section says that number and no other.

## Phases

### Phase 1: the frame map

Ranges from the Multiboot map, frames by number, holes never allocated.
Proof: a host test over recorded memory maps of QEMU and of the Lenovo, and
a boot that reports the usable total.

### Phase 2: high memory

Temporary mappings; every frame path audited and changed; the window's
users found by a build that fails to compile the old direct-map arithmetic.
Proof: a boot with 4 GiB in QEMU that allocates and frees past 32 MiB from
user space and from the page cache, and the whole suite.

### Phase 3: the tables

Sized at boot; exhaustion answered by name; the request pool waits. Proof:
a row per table that fills it and requires the named answer; the mutation
that lets `REQ_POOL` fail with `EIO` again makes the file system row fail.

### Phase 4: limits per user

`prlimit64` enforced; defaults from `init`; root's reserve. Proof: a user
that forks past its limit is refused and root still logs in.

### Phase 5: measured

The load row; the numbers on the Lenovo. Proof: the numbers, in
`hardware.md`, and the Readme's Limits section updated from them.

### Phase 6: the record

Notes, `tests/run all`, the milestone's Readme and the index entry.

## Risks

* This is the kernel's memory design changed underneath everything; each
  phase ends with the whole suite and a boot on the machine, and phase 2 is
  allowed to be the longest phase this project has had.
* Sizing tables from memory hides a bug that a fixed sixteen would show;
  every table keeps a compile-time floor the host tests run at.
* The measured number may be small. It is written down as it is.
