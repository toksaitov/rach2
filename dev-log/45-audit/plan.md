# Milestone 45: the audit

## The goal

The tree put back into a state a person can trust before the machine is
shown to students: every document says what the code does, the recovery
system writes nothing to a disk without being asked, the build's rules live
where their subject lives, and the defects the last review round found
outside its scope are fixed. Nothing here adds a feature. The milestone is
complete when the checks below pass on the final tree and the record in
`notes.md` names every finding with what was done about it.

## What the tree settles before this plan

* The recovery system repairs the disk during its own boot: `utils/init.c`
  runs `e2fsck -p` on every ext2 partition it finds before it starts anything
  (`init.c:200`). An installer that boots recovery to write a disk must be
  able to boot it without that, and so must a person who booted recovery to
  look.
* Product image rules sit in the test include: the stick image is built by
  `tests/system/suites.mk:1108`, beside the suite recipes, while the sealed
  disk is built by the root `Makefile`. The build is 807 lines in the root
  Makefile, 150 under `mk/` and 1407 in `suites.mk`.
* `mk/fingerprint.mk:32` removes a component's build directory while Make is
  still parsing, so a target that only reads can delete.
* `/proc/loop` and `/proc/interrupts` have the same inode number
  (`procfs.c:61`, `0x10E` twice).
* `rach2/sys/syscall.c` is 12071 lines.
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
* The documents drifted: `rach2/Readme.md` calls exFAT read-only where
  `exfat.c` writes; `utils/Readme.md` names retired targets; suite counts
  disagree between `tests/Readme.md`, `tests/system/Readme.md` and the
  registry, which has 73 suites and 27 in the core tier.
* `tests/system/Readme.md` calls the metal acceptance non-destructive; its
  rows write a file to the stick and copy an image to a fixed path on the
  root.
* `sys_prlimit64` (`syscall.c:11583`) accepts a limit and enforces nothing;
  it also ignores the pid it is given and accepts a resource number it does
  not know.

## Decisions

### Nothing moves that does not have to

The top-level layout stays: kernel, utilities, shell, file-system tools,
vendor ports, build fragments, tests. What is wrong is drift, duplication and
misplaced responsibility, and each is fixed where it is. No directory is
renamed and no file is moved for tidiness alone.

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
processor, and Intel advises against it (SA-00145). This is a kernel change
inside an audit because it is a correctness defect that any two
floating-point programs can show.

### Running out is not a device failure

A submission that finds the request pool full waits for a slot; only a
transfer that failed answers `EIO`, and only `EIO` makes the buffer cache
mark the device failed. This is the one piece of milestone 49 that belongs
here, because it is a wrong answer today, not a capacity question.

### Image rules go to the build, not the tests

Every rule that makes a product, the stick image among them, moves into a
build fragment under `mk/`; `suites.mk` keeps only what a suite needs. The
fingerprint's purge becomes an explicit step a build runs, not a side effect
of parsing.

### The large file is split without being changed

`syscall.c` is divided along the lines it already has, process, file system,
network and ABI arms, with no semantic change, proved by the suites passing
unchanged and by `diff` of the function bodies.

### Documents are corrected, not rewritten

This milestone corrects claims; milestone 48 restructures. A stale claim is
fixed where it stands, and a count that a document repeats from the tree is
generated or removed.

## Phases

### Phase 1: the hazards

The recovery boot's repair made optional; the fixed `/root/loop.img` path
of the acceptance rows made a temporary name under `/tmp` and cleaned up;
the `/proc` inode fixed; the request pool waiting instead of failing.
Proof: the sealed disk's bytes hashed before and after an ordinary recovery
boot and found equal, and the repair entry booted on a deliberately damaged
copy and found to change it; the acceptance rehearsal; a row that stats
both `/proc` files and requires different inodes; a row that floods a mount
with writes past the pool and finds it still writable, and the mutation
that lets the full pool answer `EIO` makes that row fail.

### Phase 2: the floating-point state

Save and restore in the switch, fork, exec and signal delivery. Proof: a
probe whose processes keep live values in x87 and XMM registers and a
non-default rounding mode across forced switches and timer preemption, fork
and signal return, and check them; the mutation that removes the restore
makes it fail, and so does the one that skips the signal frame.

### Phase 3: the build

Product rules out of `suites.mk`; the purge, and the files the fingerprint
writes while Make parses, made an explicit step; the root Makefile's
duplicated declarations and stale comments removed. The rule is that no
recipe under `tests/` makes a product, while a test may depend on one.
Proof: `make` from `distclean`, `make test`, `make -n` and an inspection
target run with a changed fingerprint and shown to delete nothing, a flag
change within one second rebuilt, a parallel build, and a check that no
recipe under `tests/` names a product.

### Phase 4: the split

`syscall.c` divided along its existing arms, one commit per extraction.
Proof: the suites, `make check-sources`, `guestcheck` and `tests/run
selfbuild` unchanged; a script that shows every function body identical
before and after is supporting evidence, since headers, macros and linkage
can change around identical bodies. Codex advised cutting this phase as
work with no benefit to a student; it stays because a 12000-line file is
the wrong shape for a reader, but it is the phase the owner may drop.

### Phase 5: the claims

Every stale claim found by the last round and by this one corrected;
`prlimit64` documented as accepted and ignored, with the pid and the
resource number checked and refused by name; enforcement is milestone 49.
Proof: `tests/run style`, and a Codex proofreading round of the touched
documents, its unit count fixed in `notes.md` first.

### Phase 6: the record

Notes closed, `tests/run all` on the final tree, the milestone's Readme and
the index entry.

## Risks

* The floating-point change touches the switch, which every process runs
  through; the eager path is the simplest one there is, and the probe is
  the proof. The phase ends with a Codex review of the switch, fork, exec
  and signal changes as one unit.
* Moving build rules can break a suite's expectations of where an artifact
  is; each move runs the suites that name the artifact.
* The split of `syscall.c` invites a "while I am here" change; the rule is
  none, and the identity check is what enforces it.
