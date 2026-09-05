# Milestone 45: the hazards

## The goal

Three defects fixed before the kernel is reworked for capacity: the
recovery system writes nothing to a disk without being asked, two
floating-point programs no longer share one register file, and a burst of
writes no longer takes a healthy mount read-only. Nothing here adds a
feature, and nothing else from the last review round is done here; the
build, the large file and the stale claims are milestone 50, after the
technical milestones that would drift them again. The milestone is complete
when the three proofs below pass on the final tree.

## What the tree settles before this plan

* The recovery system repairs the disk during its own boot: `utils/init.c`
  runs `e2fsck -p` on every ext2 partition it finds before it starts anything
  (`init.c:200`, the call at `:243`). An installer that boots recovery to
  write a disk must be able to boot it without that, and so must a person
  who booted recovery to look.
* `/proc/loop` and `/proc/interrupts` have the same inode number
  (`procfs.c:61`, `0x10E` twice).
* No code saves or restores the floating-point state across a process switch:
  `switch.s:28` switches the general registers, TLS and CR3 and nothing
  else. Two programs that both use the x87 or SSE share one register file.
  `paging.c:235` is the only code that writes CR0, for PG and WP; vector 7
  has the default handler (`isr.c:135`), which kills the process; no code
  detects FXSR or SSE, sets CR4.OSFXSR or initialises the unit; the signal
  frame (`syscall.c:8704`) has no floating-point state; fork copies none.
* A full request pool answers `EIO` (`request.c:103`), and the buffer cache
  marks the device failed on any write error (`bcache.c:151`), so a burst
  of writes can take a healthy mount read-only for good. Polled SATA does
  not use the pool (`request.c:148`).
* `tests/system/Readme.md` calls the metal acceptance non-destructive; its
  rows write a file to the stick and copy an image to a fixed path on the
  root.

## Decisions

### The recovery boot asks before it repairs

`e2fsck -p` at boot becomes a choice on the command line, `rach2.fsck`,
off by default in the recovery entries the disk and the stick carry, and
the menu gains a second recovery entry that has it on. A person who wants
the repair chooses it.

### The floating-point state is saved, eagerly

Every switch saves the outgoing process's x87 and SSE state and restores the
incoming one's: `fxsave` and `fxrstor` when CPUID reports FXSR, with
CR4.OSFXSR set at boot, and `fnsave` and `frstor` otherwise. The save area
is 512 bytes, 16-byte aligned, in the process record. Fork copies the
parent's state; exec resets it to the initial control word and MXCSR; signal
delivery saves the state on the signal frame and the return restores it.
Not lazily: a lazy switch through `CR0.TS` needs an owner, a vector 7
handler and rules for fork and exit, buys nothing measurable on one
processor, and Intel advises against it (SA-00145).

### Running out is not a device failure

A submission that finds the request pool full waits for a slot; only a
transfer that failed answers `EIO`, and only `EIO` makes the buffer cache
mark the device failed. Milestone 46 makes the pool larger and the device
interrupt-driven; this milestone makes the answer right at any size.

## Phases

### Phase 1: the recovery boot and the small things

The recovery boot's repair made optional; the fixed `/root/loop.img` path
of the acceptance rows made a temporary name under `/tmp` and cleaned up,
and the Readme's claim corrected; the `/proc` inode fixed; the request
pool waiting instead of failing. Proof: the sealed disk's bytes hashed
before and after an ordinary recovery boot and found equal, and the repair
entry booted on a deliberately damaged copy and found to change it; the
acceptance rehearsal; a row that stats both `/proc` files and requires
different inodes; a row that floods a mount with writes past the pool and
finds it still writable, and the mutation that lets the full pool answer
`EIO` makes that row fail.

### Phase 2: the floating-point state

Save and restore in the switch, fork, exec and signal delivery. Proof: a
probe whose processes keep live values in x87 and XMM registers and a
non-default rounding mode across forced switches and timer preemption, fork
and signal return, and check them; the mutation that removes the restore
makes it fail, and so does the one that skips the signal frame. The phase
ends with a Codex review of the switch, fork, exec and signal changes as
one unit.

### Phase 3: the record

Notes, `tests/run all`, a boot on the Lenovo with the new kernel through
`bootctl`, the touched Readmes corrected in the same commits, the
milestone's Readme and the index entry.

## Risks

* The floating-point change touches the switch, which every process runs
  through; the eager path is the simplest one there is, and the probe is
  the proof.
* A request that waits can wait on a device that never answers; the wait
  has the deadline the request layer already has for sleeping devices.
