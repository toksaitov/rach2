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
  `switch.s`, `process.c` and `isr.c` have no `fnsave`, `fxsave` or `clts`.
  Two programs that both use the x87 or SSE share one register file.
* The documents drifted: `rach2/Readme.md` calls exFAT read-only where
  `exfat.c` writes; `utils/Readme.md` names retired targets; suite counts
  disagree between `tests/Readme.md`, `tests/system/Readme.md` and the
  registry, which has 73 suites and 27 in the core tier.
* `tests/system/Readme.md` calls the metal acceptance non-destructive; its
  rows write a file to the stick and copy an image to a fixed path on the
  root.
* The `prlimit64` path accepts a limit and enforces nothing (`syscall.c`,
  the `prlimit64` arm).

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

### The floating-point state is saved

The switch saves and restores the x87 and SSE state per process with
`fxsave` and `fxrstor` when the processor has them and `fnsave` and `frstor`
when it does not, lazily through `CR0.TS` so a process that never touches
the unit pays nothing. This is a kernel change inside an audit because it is
a correctness defect that any two floating-point programs can show.

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
the `/proc` inode fixed. Proof: `tests/run sealed` with the recovery entry
booted and no `e2fsck` record in its log; the acceptance rehearsal; a row
that stats both `/proc` files and requires different inodes.

### Phase 2: the floating-point state

Save and restore in the switch, lazily. Proof: a probe that forks two
processes each computing a known floating-point sequence and checks their
results; the mutation that removes the restore makes it fail.

### Phase 3: the build

Product rules out of `suites.mk`; the purge explicit; the root Makefile's
duplicated declarations and stale comments removed. Proof: `make` from
`distclean`, `make test`, and a check that no target under `tests/` names a
product.

### Phase 4: the split

`syscall.c` divided along its existing arms, one commit per extraction.
Proof: the suites unchanged and a script that shows every function body
identical before and after.

### Phase 5: the claims

Every stale claim found by the last round and by this one corrected;
`prlimit64` either enforced for the limits the kernel has or documented as
accepted and ignored. Proof: `tests/run style`, and a Codex proofreading
round of the touched documents, its unit count fixed in `notes.md` first.

### Phase 6: the record

Notes closed, `tests/run all` on the final tree, the milestone's Readme and
the index entry.

## Risks

* The floating-point change touches the switch, which every process runs
  through; the lazy path has a known shape and the probe is the proof.
* Moving build rules can break a suite's expectations of where an artifact
  is; each move runs the suites that name the artifact.
* The split of `syscall.c` invites a "while I am here" change; the rule is
  none, and the identity check is what enforces it.
