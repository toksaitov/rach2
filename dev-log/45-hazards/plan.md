# Milestone 45: the hazards

## The goal

Three defects fixed before the kernel is reworked for capacity: the
recovery system writes nothing to a disk without being asked, two
floating-point programs no longer share one register file, and a burst of
writes no longer takes a healthy mount read-only. Nothing here adds a
feature, and nothing else from the last review round is done here; the
build, the large file and the stale claims are milestone 52, after the
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
* A full request pool answers `EIO` (`request.c:103`), a flush submission
  too (`request.c:208`), and the buffer cache marks the device failed on
  any write error (`bcache.c:151`), so a burst of writes can take a healthy
  mount read-only for good. A request past its deadline is quarantined and
  its slot returns only when a late completion arrives (`request.c:296`,
  `:339`), so a device that never answers can hold every slot. Polled
  SATA does not use the pool (`request.c:148`). ext2 serialises writes
  under its mount lock (`ext2.c:2064`), so flooding one mount from
  sixteen processes does not fill the pool.
* Every `tests/run NAME` prepares every product and tool first
  (`tests/run:159`, `suites.mk:151`), so the smallest suite is not cheap.
* A disk recovery trial consumes `next_entry` and writes `grubenv` before
  the kernel starts (`mkdisk_grub.sh:358`).
* Signal return treats its frame as untrusted and sanitises the registers
  it restores (`syscall.c:8770`).
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

A submission, a flush included, that finds the request pool full waits
for a slot, and its transfer deadline starts when the transfer starts,
not when it joined the queue (`request.c:47` starts it at the slot
today); only a transfer that failed answers `EIO`, and only `EIO` makes
the buffer cache mark the device failed. A waiter cannot wait on
quarantined slots for ever: when every slot is quarantined the device is
declared failed, the waiters are released with `EIO`, and that is the one
case where the answer is the device's. Milestone 46 replaces the pool
with allocated requests and milestone 47 makes the disk interrupt-driven;
this milestone makes the answer right at any size, proved on a sleeping
device, since polled SATA bypasses the pool, with a boot switch that
shrinks the pool so the row can see a request wait.

### The small suite is cheap

`tests/run NAME` prepares what NAME needs and no more, so the checks a
phase runs on the way cost what they should. The baseline of
`tests/run all`, its host, worker count and time, is recorded in
`notes.md` before anything else changes, for milestone 52 to hold to.

## Phases

### Phase 1: the recovery boot and the small things

The baseline recorded; `tests/run NAME` preparing only its own needs; the
recovery boot's repair made optional; the fixed `/root/loop.img` path of
the acceptance rows made a temporary name under `/tmp` and cleaned up,
and the Readme's claim corrected; the `/proc` inode fixed; the request
pool waiting instead of failing. Proof: `tests/run host` timed before and
after; the sealed disk's bytes hashed before and after an ordinary
recovery boot from the stick and found equal, so the boot loader's own
`grubenv` write is not in the comparison, and the repair entry booted on
a deliberately damaged copy and found to change it; the acceptance
rehearsal; a row that stats both `/proc` files and requires different
inodes; a row with the pool shrunk to two that shows a request waiting,
completing, and the mount still writable, a flush among them, and the
same row against a device that never answers finding `EIO` and no waiter
left; the mutation that lets the full pool answer `EIO` makes the first
fail and the one that lets a waiter sleep on quarantined slots makes the
second hang, which the harness's deadline reports.

### Phase 2: the floating-point state

Save and restore in the switch, fork, exec and signal delivery, the
restored state on signal return sanitised as the general registers are
today. Proof: a probe whose processes keep live values in x87 and XMM
registers and a non-default rounding mode across forced switches and
timer preemption, fork, a nested signal handler and signal return, and
check them; exec shown to reset the state; a damaged frame refused; the
same probe with FXSR disabled on the boot line for the `fnsave` path; the
mutation that removes the restore makes it fail, and so does the one that
skips the signal frame.

### Phase 3: the close

Proof, once and in this order: build from clean; the full set; the new
kernel on the Lenovo through `bootctl` and the SSH capstone; everything that
comes up fixed; the whole milestone's diff to Codex in units of six files,
most serious first; every finding fixed; the full set again. The Readmes the
milestone changed carry the facts in the same commits as the code and Codex
writes their prose in the same round. Problems found on the way that this
milestone did not set out to fix are already in
`dev-log/50-shortcomings/plan.md` with file and line, and none is fixed here
unless the milestone cannot close without it. Then notes, the milestone's
Readme and the index entry.

## Risks

* The floating-point change touches the switch, which every process runs
  through; the eager path is the simplest one there is, and the probe is
  the proof.
* A request that waits can wait on a device that never answers; the wait
  has the deadline the request layer already has for sleeping devices.
