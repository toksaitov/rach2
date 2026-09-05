# Milestone 46: capacity

## The goal

The machine serves twenty students doing real work at once: each logged in
over SSH, editing, compiling with `tcc` and `make`, running what they
built, some of them all at the same moment. Nothing in the kernel is what
stops them: it uses the memory the machine has, up to the 4 GiB a 32-bit
kernel addresses; the disk does not hold the processor while it works; and
no table of sixteen decides how many people can log in, how many files a
build can open or how many pipes a shell can make. A process, a descriptor,
a pty, a pipe, a socket, a connection or a disk request that is not there
is waited for or refused by name, never turned into a disk error or a
silent failure, and one person cannot take the machine from everyone else.
SMP is not in scope. The milestone is complete when twenty sessions on the
Lenovo run a fixed mix of building and running for an hour, the numbers are
in `hardware.md`, and the Readme's Limits section says them.

## What the tree settles before this plan

* The kernel identity-maps a 32 MiB window (`paging.h:15`, `paging.c:160`);
  user addresses begin right above it at `USER_ADDR_MIN` (`usercopy.h:22`),
  which only the range checks use; programs are linked at `0x08048000`,
  128 MiB; MMIO sits near `0xC0500000` on the Lenovo and the KRFFS
  page-cache window begins at `0xD0000000` (`pagecache.h:15`). The frame
  code reduces the Multiboot map to its highest available address and caps
  it (`frame.c:60`); `paging.c:150` then marks that whole range free,
  holes included. Every kernel path that touches a frame does so through
  the identity map: zeroing at `paging.c:375`, copy-on-write at
  `paging.c:460`, the loader at `elf.c:209`, and the page tables, kernel
  stacks, DMA buffers and every kernel object.
* The page cache's 512 MiB is an address window; its resident capacity is
  4 MiB, 1024 frames (`pagecache.h:25`). The buffer cache has 64 buffers.
  Frame reference counts are one byte and a saturated count pins the frame
  for good (`paging.c:871`).
* The AHCI driver polls with interrupts off and one command slot
  (`ahci.c:166`, `:544`); every disk transfer holds the processor until it
  completes, so twenty people's builds wait on each other's reads.
* The tables: `MAX_PROCS` 16 (`process.h:11`), `MAX_FDS` 16 (`fd.h:21`),
  `OFD_POOL_SIZE` is `16 * MAX_FDS` (`fd.c:14`), `PTY_MAX` 16 (`pty.h:35`),
  `MAX_PIPES` 16 (`pipe.c:32`), `MAX_SOCKETS` 16 (`socket.h:26`),
  `MAX_TCP_CONNS` 16 (`tcp.h:26`), `REQ_POOL` 16 (`request.c:22`),
  `VFS_MOUNTS` 8, `BCACHE_BUFFERS` 64, 64 ext2 nodes held per mount
  (`ext2.h:7`), eight UDP bindings, eight ARP entries, 4 KiB TCP buffers.
  A process carries 512 VMA entries and a 16 KiB kernel stack; wait queues
  are arrays sized by `MAX_PROCS`; the process layout is read by assembly.
* The scheduler is round-robin at 100 Hz (`sched.c:32`). One processor.
  No threads.
* Milestone 45 made a full request pool wait rather than answer `EIO`.
* Dropbear forks once per connection, before authentication and as root
  (`svr-main.c:308`), and once per session; a logged-in student is two
  processes before a shell runs anything.
* `prlimit64` accepts and ignores (`syscall.c:11583`).
* PAE would give the kernel physical addresses above 4 GiB through a second
  paging format with 64-bit entries; it would give a process nothing, and
  every DMA and mapping path would change with it.

## Decisions

### Physical memory is accounted first

A frame allocator that takes the Multiboot map as a list of usable ranges
and never hands out a hole. Frames are named by number, not by pointer.
This comes before any window grows, because the present allocator would
give out firmware and PCI ranges as RAM once the window did.

### The identity map grows to 128 MiB; everything else is high

The identity map grows to where programs are linked, 128 MiB, and
`USER_ADDR_MIN` moves with it; the address-space code's placement of the
heap, mappings and stack is checked to lie above it. That is a fourfold
low pool for what must stay low: page tables, kernel stacks, DMA buffers
and kernel objects. Every other frame, user pages and the page cache and
the buffer cache, comes from high memory through temporary kernel
mappings, the way a 32-bit kernel has always reached it: the three user
paths change, zeroing, the copy-on-write copy and the loader, and the two
caches map their frames into their own windows. The map cannot move above
user space without relinking the kernel and every MMIO and cache mapping,
and that is not worth it. User address space above 128 MiB is unchanged,
so no executable changes. The processor's own limit, four gigabytes of
physical address, is the ceiling; PAE is not in this milestone.

### The caches hold what the memory allows

The page cache's resident capacity and the buffer cache are sized at boot
from the memory found, with a floor of today's numbers, and they give
frames back under pressure. The reference count is widened before the
sharing this allows can saturate it.

### The disk is driven by interrupts

The AHCI driver enables interrupts, uses the port's command slots, and
completes a request from its handler through the request layer's sleeping
path, which already exists for such a device. A process that waits on the
disk sleeps; the processor runs someone else. The polled path stays for
the recovery kernel's raw window and for boot, where nothing else runs.

### The tables are raised, together

Sixteen is over. Each table gets a bound in one header so they are chosen
together, with the dependent storage sized off the same numbers in fact:

* processes 256, and the wait queues and the process layout the assembly
  reads with them;
* descriptors 256 per process, the open-file pool sized off it;
* ptys 128, pipes 512, sockets 256, TCP connections 256, UDP bindings 64,
  ARP entries 64, TCP buffers 16 KiB;
* disk requests 128, mounts 32, ext2 nodes per mount sized from memory
  with a floor of 64.

Each table has a counter under `/proc` for the times it was full. A
socket, a pipe or a pty that is not there is `ENOMEM` or `EAGAIN` by name.
The host tests keep running at sixteen through the same header, so a bug
that sixteen would show is still shown.

### A person is limited

`prlimit64` enforces three limits with the meaning they have elsewhere:
`RLIMIT_NPROC` counts the processes of one real user across the machine,
`RLIMIT_NOFILE` and `RLIMIT_AS` bound one process. Each has a soft and a
hard value, both inherited across fork and exec, and only root raises a
hard one; a `setuid` to another user counts the process against that user.
`init` sets the defaults for the accounts the installer makes: 64
processes, 256 files, 512 MiB of address space. An administrator's way in
is the console, which needs no socket and no connection slot, and that is
the guarantee this milestone makes. Over SSH the guarantee is narrower and
stated: dropbear's unauthenticated children are bounded by a connection
cap in the server's own configuration, so a flood of connections cannot
take every process, but a user who fills the socket table can still keep
root out until the sockets time out.

### What is measured

The suites gain a load row: N sessions over SSH in QEMU doing a fixed mix,
an edit, a build of a small program with `make`, a run, a pipeline. The
number the Lenovo reaches with the mix running for an hour is what
`hardware.md` records, twenty is the number this milestone must reach, and
the Readme's Limits section says the measured number and no other.

## Phases

### Phase 1: the frame map

Ranges from the Multiboot map, frames by number, holes never allocated,
the kernel, the modules, the boot data and the allocator's own tables
reserved. Proof: a host test over the recorded maps of QEMU and of the
Lenovo and over synthetic maps with holes, overlapping entries and a range
crossing 4 GiB; and a boot that reports the usable total.

### Phase 2: high memory

The identity map to 128 MiB; temporary mappings; the three user-frame
paths changed; the identity-map arithmetic they used made uncompilable so
no fourth path is missed; the two caches on high frames and sized from
memory; the reference count widened. Proof: a boot with 4 GiB in QEMU
whose probe fills pages beyond the low pool with a pattern and reads it
back, forces copy-on-write on them, execs and frees and reuses them, and
exhausts the low pool while high frames remain and still forks; a row
that shares one frame past 255 times and finds it freed afterwards; the
page cache shown holding more than 4 MiB; the whole suite; a boot on the
Lenovo; the phase ends with a Codex review of the memory changes as one
unit.

### Phase 3: the disk

Interrupt-driven AHCI through the request layer's sleeping path, with the
command slots in use. Proof: a row that reads from two processes at once
and shows the second progressing while the first waits on the disk; the
disk suites; the metal acceptance on the Lenovo; the phase ends with a
Codex review of the driver change.

### Phase 4: the tables

The bounds chosen together; exhaustion answered by name and counted.
Proof: a row per table that fills it and requires the named answer and the
counter's rise; twenty sessions logged in over SSH in QEMU at once.

### Phase 5: limits per user

`prlimit64` enforced as decided; defaults from `init`; the connection cap.
Proof: a user that forks past its limit is refused while another user is
not; a connection flood from one host while root logs in on the console;
the phase ends with a Codex review of the limits as one unit.

### Phase 6: measured

The load row; twenty sessions on the Lenovo for an hour; the numbers.
Proof: the numbers, in `hardware.md`, and the Readme's Limits section
updated from them.

### Phase 7: the record

Notes, `tests/run all`, the kernel and subsystem Readmes changed in the
same commits as the code they describe, the milestone's Readme and the
index entry.

## Risks

* This is the kernel's memory design changed underneath everything; each
  phase ends with the whole suite and a boot on the machine, and phase 2 is
  allowed to be the longest phase this project has had.
* Interrupt-driven AHCI on the Lenovo's controller is new ground; the MSI
  path the network card uses is the model, and the polled path stays as
  the fallback a boot switch can choose.
* Twenty builds at once on one processor will be slow however the kernel
  is shaped; the measurement says how slow, and that number is what the
  Readme says.
