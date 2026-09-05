# Milestone 52: the audit

## The goal

The tree put back into a state a person can trust, after the technical
milestones and once, so nothing drifts it again before students see it,
and the machine and the stick carrying the result:
every document says what the code does, the build's rules live where
their subject lives, the largest file is a size a reader can hold, and the
documents a developer reads have one shape, one voice and no claim the
code does not keep. The root Readme says what the project is, what it
supports, how to build and run it, and where everything else is. Each
component's Readme has the same parts in the same order. History stays in
`dev-log/` and nowhere else. The milestone is complete when the checks
below pass on the final tree, every claim in every Readme has an owner, a
Codex round finds nothing stale, and a person new to the tree can find the
answer to any question the milestone's own checklist asks in under a
minute, or the claim is dropped; the test set covers the system as it now
is, runs in parallel, and takes no longer than it did before milestone 45
plus what the new hardware arrangements need; the build has no rule left
over from a milestone that no longer needs it; the whole set of images is
on the notebook's disk and the stick carries recovery and the installer;
and the comments in the source and the support files have been read by
Codex against the code they describe.

## What the tree settles before this plan

* Product image rules sit in the test include: the stick image is built by
  `tests/system/suites.mk:1108`, beside the suite recipes, while the sealed
  disk is built by the root `Makefile`. The build is 807 lines in the root
  Makefile, 150 under `mk/` and 1407 in `suites.mk`.
* `mk/fingerprint.mk:25` writes files and removes a component's build
  directory while Make is still parsing, so a target that only reads can
  delete.
* `rach2/sys/syscall.c` is 12071 lines.
* The documents drifted before milestone 45 and the five milestones since
  changed the memory, the disks, the devices, the installer and the
  manuals: the last
  round found `rach2/Readme.md` calling exFAT read-only where `exfat.c`
  writes, `utils/Readme.md` naming retired targets, and suite counts
  disagreeing between `tests/Readme.md`, `tests/system/Readme.md` and the
  registry. Each milestone corrected the Readmes it touched in the same
  commits; this milestone reads everything once.
* There are 70 files named `Readme.md` outside `build/` and the vendored
  trees, and 79 with the `NAME.Readme.md` form counted; the `dev-log/`
  records are among them and are history, outside this milestone. The
  root one does five jobs: introduction, architecture, quick start, the
  demonstration walk-through, and the administration of one laptop.
* The component Readmes differ in shape: some are a contract with its
  failure behaviour and its proofs, some are a narrative of how a thing came
  to be, some are an inventory that repeats what a manifest or a registry
  already lists.
* Milestone 51 gave the machine manual pages: `install(8)`, `bootctl(8)`,
  `reflash(8)` and `recovery(7)` are where a procedure now lives.
* Milestone 46 classified every fixed bound that stayed in the kernel;
  the tools were not classified: `guestcheck` assumes 16 descriptors
  (`guestcheck.c:38`), `bootslot` has fixed FAT workspaces
  (`bootslot.h:28`), the sealed image fixes four sizes (`Makefile:624`).
* `CLAUDE.md` makes Codex the writer and checker of prose since
  2026-09-05: the facts and the claim inventory are prepared here, the
  prose is Codex's, and every claim in what comes back is checked here.
* `tools/verify/prosecheck.c` decides the mechanical half of the register
  rule; a person decides the rest.

## Decisions

### Every remaining bound is classified

Every fixed number left in the kernel, the tools, the image builders and
the procedures is found and classified: physical, format or ABI, policy
and configurable, safety invariant, or unfinished work. The first four
get their reason beside them; the fifth gets an entry in `notes.md` and
a place in the next plan. An old helper or document that still imposes a
limit the kernel no longer has is the fault this phase exists to find.

### Nothing moves that does not have to

The top-level layout stays: kernel, utilities, shell, file-system tools,
vendor ports, build fragments, tests. What is wrong is drift, duplication and
misplaced responsibility, and each is fixed where it is. No directory is
renamed and no file is moved for tidiness alone.

### Image rules go to the build, not the tests

Every rule that makes a product, the stick images among them, moves into a
build fragment under `mk/`; `suites.mk` keeps only what a suite needs. The
rule is that no recipe under `tests/` makes a product, while a test may
depend on one. The fingerprint's purge, and the files it writes while Make
parses, become an explicit step a build runs.

### The large file is split without being changed

`syscall.c` is divided along the lines it already has, process, file system,
network and ABI arms, with no semantic change, proved by the suites, the
guest build and the source checks passing unchanged. Codex advised cutting
this as work with no benefit to a student; it stays because a 12000-line
file is the wrong shape for a reader, but it is the phase the owner may
drop.

### An inventory of claims before a word is rewritten

Every Readme is read against the tree and each claim gets an owner and an
evidence type: a file and function for what code does; a measured record
under `dev-log/` for a hardware fact; a build rule or the doctor script
for a prerequisite; `CLAUDE.md` for a policy. A claim with none is removed
or made true. A claim two documents make is made once and linked from the
other. The inventory is a file in this milestone's directory and the
rewrite is done from it.

### Four kinds of document, each with a fixed shape

The root Readme: purpose, what it supports, quick start, limits, and a map.
The administration prose is split by what it is: the general procedures
are the manual pages milestone 51 wrote; the measured facts of the one
installed machine, its identity, geometry and offsets, stay in the
milestone records under `dev-log/`; the root Readme keeps a short section
that points to both, and `CLAUDE.md`'s Hardware section, which today sends
an operator to the root Readme, is updated in the same commit. The kernel
Readme: an architectural map, one paragraph per subsystem with a link. A
component Readme: what it owns, its interface, its invariants, what it
does on failure, and what proves it, in that order, then, where the
component has one, how it is built and ported; what is excluded is
history, not current instructions. The `dev-log/` records are history and
are not rewritten to describe today.

### Inventories are generated

A list a document repeats from the tree today, the suites, the programs,
the system calls, is produced by the tool that already reads the registry
or removed; a document does not carry a count a registry already holds. No
new tool is written for a list that appears once.

### Codex writes the documents from the inventory, and reads the comments

The claim inventory is the input: for each document, its shape and its
claims with their owners. Codex writes each document from that, in units
of six, and what comes back is checked here claim by claim before it is
committed. Codex also reads every comment in the source and the support
files against the code beside it, in the same units, and reports the ones
that are stale, that narrate, or that use the register `CLAUDE.md`
forbids; those are fixed here.

### The test set is made lean and the build is made clean

Every suite is listed with what it proves and how long it runs; a suite
that proves what another proves is merged or removed; the new hardware
arrangements, memory sizes, two disks, two controllers, two cards, get
rows in existing suites where they fit and new suites only where they do
not; the tiers are rebalanced so the core tier still answers in the time
it did. Every rule in the build is listed with its owner; a rule with
none, or one kept for a milestone that ended, goes.

### The images go to the machine and the stick

The last act of the milestone is the deployment: the stick rebuilt with
recovery and the installer, booted and shown to work; the whole set of
images installed on the notebook's disk through the installer under the
hardware rule; the demo checked on the machine.

## Phases

### Phase 1: the build

Product rules out of `suites.mk`; the purge and the parse-time writes made
an explicit step; the root Makefile's duplicated declarations and stale
comments removed. Proof: `make` from `distclean`, `make test`, `make -n`
and an inspection target run with a changed fingerprint and shown to
delete nothing, a flag change within one second rebuilt, a parallel build,
and a check that no recipe under `tests/` names a product.

### Phase 2: the split

`syscall.c` divided along its existing arms, one commit per extraction.
Proof: the suites, `make check-sources`, `guestcheck` and `tests/run
selfbuild` unchanged; a script that shows every function body identical
before and after is supporting evidence, since headers, macros and linkage
can change around identical bodies.

### Phase 3: the test set and the build

The suite and rule inventories; the merges and removals; the new rows;
the tiers. Proof: the full set's time recorded against the record from
before milestone 45; every suite in the registry named in the inventory.

### Phase 4: the bounds

The classification, with the reasons written into the headers, the
scripts and the Makefile; the tools' own limits lifted where the kernel's
were. Proof: a host check that lists every numeric bound in the tree with
its class, and refuses one without.

### Phase 5: the inventory

Every Readme read; the claim inventory written; the criticism round on
Codex, its units fixed in `notes.md` first. Proof: the inventory names an
owner for every claim or a fix.

### Phase 6: the pilot

The root Readme, the kernel Readme and one component Readme, `vfs.Readme.md`,
written by Codex from the inventory into the shapes above and checked
here. Proof: the owner reads the three and accepts the shape before it
goes further.

### Phase 7: the rest

Every component Readme into its shape; the administration prose split as
decided; the generated inventories. Proof: `tests/run style`, the
generator's output matching the tree, a link check, and the checklist: a
file of questions a newcomer asks, each answered by a named place in the
documents, worked through by the owner with a clock; the time is recorded
in `notes.md` and the one-minute claim is kept or dropped by it.

### Phase 8: the comments

Codex over every source and support file, six a unit, reading each
comment against its code; the stale, the narrating and the wrong register
fixed here. Proof: `tests/run style` and a second Codex pass over the
files that changed finding nothing.

### Phase 9: the close and the deployment

Once: build from clean; the full set; the whole milestone's diff to Codex
in units of six files; every finding fixed; the full set again. Then the
stick rebuilt and booted, the notebook installed through the installer
under the hardware rule, the demo checked on it. Then notes, the
milestone's Readme and the index entry, and the report to the owner that
everything is done, with nothing deferred.

## Risks

* Moving build rules can break a suite's expectations of where an artifact
  is; each move runs the suites that name the artifact.
* The split of `syscall.c` invites a "while I am here" change; the rule is
  none, and the identity check is what enforces it.
* A rewrite is where a true sentence becomes a smooth false one; the
  inventory is the defence, and a claim without an owner does not go in.
* The shape may not fit a Readme that describes a tool rather than a
  subsystem; the pilot is where that is found, before the rest is done.
