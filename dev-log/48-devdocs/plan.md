# Milestone 48: the developer documentation

## The goal

The documents a developer reads have one shape, one voice and no claim the
code does not keep. The root Readme says what the project is, what it
supports, how to build and run it, and where everything else is. One
document says how the installed machine is operated. Each component's
Readme has the same parts in the same order. History stays in `dev-log/`
and nowhere else. The milestone is complete when every claim in every
Readme has an owner in the code, a Codex round finds nothing stale, and a
person new to the tree can find the answer to any question the milestone's
own checklist asks in under a minute.

## What the tree settles before this plan

* There are 70 files named `Readme.md` outside `build/` and the vendored
  trees, and 79 with the `NAME.Readme.md` form counted; the `dev-log/`
  records are among them and are history, outside this milestone. The
  root one is 240 lines and does five jobs:
  introduction, architecture, quick start, the demonstration walk-through,
  and the administration of one laptop.
* The component Readmes differ in shape: some are a contract with its
  failure behaviour and its proofs, some are a narrative of how a thing came
  to be, some are an inventory that repeats what a manifest or a registry
  already lists.
* Milestone 45 corrected the claims the last round caught; it did not
  restructure.
* `CLAUDE.md` says a delegate reviews and proofreads and never writes a
  document, and that the rewriting is done here so the tree keeps one voice.
  The owner asked for Codex's criticism and structure "up to a full
  rewrite"; Codex's own answer was that it criticises and reviews and the
  writing stays here unless the owner changes the rule.
* `tools/verify/prosecheck.c` decides the mechanical half of the register
  rule; a person decides the rest.

## Decisions

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
The administration prose is split by what it is: the general procedures,
how a machine is installed, updated and recovered, go to milestone 47's
`install(8)`, `bootctl(8)`, `reflash(8)` and `recovery(7)`; the measured
facts of the one installed machine, its identity, geometry and offsets,
stay in the milestone records under `dev-log/`; the root Readme keeps a
short section that points to both, and `CLAUDE.md`'s Hardware section,
which today sends an operator to the root Readme, is updated in the same
commit. The kernel Readme: an architectural map, one paragraph per
subsystem with a link. A component Readme: what it owns, its interface,
its invariants, what it does on failure, and what proves it, in that
order, then, where the component has one, how it is built and ported;
what is excluded is history, not current instructions. The `dev-log/`
records are history and are not rewritten to describe today.

### Inventories are generated

A list a document repeats from the tree today, the suites, the programs,
the system calls, is produced by the tool that already reads the registry
or removed; a document does not carry a count a registry already holds. No
new tool is written for a list that appears once.

### Codex criticises first and proofreads last; the writing is here

The round at the start takes at most six files per unit and asks for the
structure it would use and every claim it doubts; the round at the end
proofreads. Between them, one voice. If the owner wants Codex as the author
of a document, that is a change to `CLAUDE.md` made before the document is
written, not an exception made during.

## Phases

### Phase 1: the inventory

Every Readme read; the claim inventory written; the criticism round on
Codex, its units fixed in `notes.md` first. Proof: the inventory names an
owner for every claim or a fix.

### Phase 2: the pilot

The root Readme, the kernel Readme and one component Readme, `vfs.Readme.md`,
rewritten into the shapes above. Proof: the owner reads the three and
accepts the shape before it goes further.

### Phase 3: the rest

Every component Readme into its shape; the administration prose split as
decided; the generated inventories. Proof: `tests/run style`, the
generator's output matching the tree, a link check, and the checklist: a
file of questions a newcomer asks, each answered by a named place in the
documents, worked through by the owner with a clock; the time is recorded
in `notes.md` and the one-minute claim is kept or dropped by it.

### Phase 4: the proofreading round

Codex over the final documents, six files a unit, wording proposed and
applied here. Proof: no claim it doubts survives unanswered in `notes.md`.

### Phase 5: the record

Notes, `tests/run all`, the milestone's Readme and the index entry.

## Risks

* A rewrite is where a true sentence becomes a smooth false one; the
  inventory is the defence, and a claim without an owner does not go in.
* The shape may not fit a Readme that describes a tool rather than a
  subsystem; the pilot is where that is found, before the rest is done.
