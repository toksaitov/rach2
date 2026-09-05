# Milestone 52: the audit

## The goal

Update the build, the source structure, the documents and the comments to match the completed system, once, after the technical milestones, and deploy the resulting images to the notebook and the stick: every document says what the code does, the build's rules live where their subject lives, the largest source file is split by interface, and the documents a developer reads have one shape, one voice and no claim the code does not keep. The root Readme says what the project is, what it supports, how to build and run it, and where everything else is. Each component's Readme has the same parts in the same order. History stays in `dev-log/` and nowhere else. The milestone is complete when the checks below pass on the final tree, every claim in every Readme has an owner, a Codex round finds nothing stale, and a person new to the tree can find the answer to any question the milestone's own checklist asks in under a minute, or the claim is dropped; the test set covers the system as it now is, runs in parallel, and takes no longer than it did before milestone 45 plus what the new hardware arrangements need; the build has no rule left over from a milestone that no longer needs it; the whole set of images is on the notebook's disk and the stick carries recovery and the installer; and the comments in the source and the support files have been read by Codex against the code they describe.

## What the tree settles before this plan

* Product image rules sit in the test include: the stick image is built by `tests/system/suites.mk:1108`, beside the suite recipes, while the sealed disk is built by the root `Makefile`. The build is 807 lines in the root Makefile, 150 under `mk/` and 1407 in `suites.mk`.
* `mk/fingerprint.mk:25` writes files and removes a component's build directory while Make is still parsing, so a target that only reads can delete.
* `rach2/sys/syscall.c` is 12071 lines.
* The documents drifted before milestone 45 and the five milestones since changed the memory, the disks, the devices, the installer and the manuals: the last round found `rach2/Readme.md` calling exFAT read-only where `exfat.c` writes, `utils/Readme.md` naming retired targets, and suite counts disagreeing between `tests/Readme.md`, `tests/system/Readme.md` and the registry. Each milestone corrected the Readmes it touched in the same commits; this milestone reads everything once.
* There are 70 files named `Readme.md` outside `build/` and the vendored trees, and 79 with the `NAME.Readme.md` form counted; the `dev-log/` records are among them and are history, outside this milestone. The root one does five jobs: introduction, architecture, quick start, the demonstration walk-through, and the administration of one laptop.
* The component Readmes differ in shape: some are a contract with its failure behaviour and its proofs, some are a narrative of how a thing came to be, some are an inventory that repeats what a manifest or a registry already lists.
* Milestone 51 wrote the manual pages the images contain: `rach2-install(8)`, `bootctl(8)`, `reflash(8)` and `recovery(7)` are where a procedure now lives.
* Milestone 46 classified every fixed bound that stayed in the kernel; the tools were not classified: `guestcheck` assumes 16 descriptors (`guestcheck.c:38`), `bootslot` has fixed FAT workspaces (`bootslot.h:28`), the sealed image fixes four sizes (`Makefile:624`).
* `CLAUDE.md` makes Codex the writer and checker of prose since 2026-09-05: the facts and the claim inventory are prepared here, the prose is Codex's, and every claim in what comes back is checked here.
* `tools/verify/prosecheck.c` decides the mechanical half of the register rule; a person decides the rest.

## Decisions

### Every remaining bound is classified

Find and classify every remaining capacity limit in the kernel, the tools, the image builders and the procedures: physical, format or ABI, policy and configurable, or safety invariant, each with its reason beside it. A bound that is none of those is unfinished work, and unfinished work is an entry in milestone 50's list, fixed before this milestone's documents are accepted; there is no next plan. A tool's own limit that obstructed milestone 46 or 47 was lifted there, not saved for here. An old helper or document that still imposes a limit the kernel no longer has is the fault this phase exists to find, and the inventory is reviewed by a person, not produced by a new checker.

### Nothing moves that does not have to

The top-level layout stays: kernel, utilities, shell, file-system tools, vendor ports, build fragments, tests. Correct stale statements, remove duplication and put each build rule in its owning component. No directory is renamed and no file is moved for tidiness alone.

### Image rules go to the build, not the tests

Every rule that makes a product, the stick images among them, moves into a build fragment under `mk/`; `suites.mk` keeps only what a suite needs. The rule is that no recipe under `tests/` makes a product, while a test may depend on one. The fingerprint's purge, and the files it writes while Make parses, become an explicit step a build runs.

### The large file is split without being changed

`syscall.c` is divided along the lines it already has, process, file system, network and ABI arms, with no semantic change, proved by the suites, the guest build and the source checks passing unchanged. Split `syscall.c` to make related interfaces easier to find; the owner may omit this phase.

### An inventory of claims before a word is rewritten

Every Readme is read against the tree and each claim gets an owner and an evidence type: a file and function for what code does; a measured record under `dev-log/` for a hardware fact; a build rule or the doctor script for a prerequisite; `CLAUDE.md` for a policy. A claim with none is removed or made true. A claim two documents make is made once and linked from the other. The inventory is a file in this milestone's directory and the rewrite is done from it.

### Four kinds of document, each with a fixed shape

The root Readme: purpose, what it supports, quick start, limits, and a map. The administration prose is split by what it is: the general procedures are the manual pages milestone 51 wrote; the measured facts of the one installed machine, its identity, geometry and offsets, stay in the milestone records under `dev-log/`; the root Readme keeps a short section that points to both, and `CLAUDE.md`'s Hardware section, which today sends an operator to the root Readme, is updated in the same commit. The kernel Readme: an architectural map, one paragraph per subsystem with a link. A component Readme: what it owns, its interface, its invariants, what it does on failure, and what proves it, in that order, then, where the component has one, how it is built and ported; what is excluded is history, not current instructions. The `dev-log/` records are history and are not rewritten to describe today.

### Inventories are generated

Generate repeated inventories and counts, the suites, the programs, the system calls, from their registries with the tool that already reads them. Remove lists that do not need to be repeated. No new tool is written for a list that appears once.

### Codex writes the documents from the inventory, and reads the comments

The claim inventory is the input: for each document, its shape and its claims with their owners. Codex writes each document from that, in units of six, and what comes back is checked here claim by claim before it is committed. Codex also reads every comment in the source and the support files against the code beside it, in the same units, and reports the ones that are stale, that narrate, or that use the register `CLAUDE.md` forbids; those are fixed here.

### The test set is made lean and the build is made clean

Every suite is listed with what it proves and how long it runs; a suite that proves what another proves is merged or removed; the new hardware arrangements, memory sizes, two disks, two controllers, two cards, get rows in existing suites where they fit and new suites only where they do not, and two suites of one subsystem with different failure modes are not duplicates; the tiers are rebalanced so the core tier still answers in the time it did. The runner today serialises style, host, USB, USB disk, sealed and mount work (`tests/run:167`) and budgets workers by processor alone; it gets a worker budget and a memory budget, since the memory matrix boots at 4 GiB, isolated artifacts per worker, console parsing that does not mistake one suite's output for another's, and deadlines sized to the row. The allowed time for the full set is the baseline milestone 45 recorded plus a quarter, on the same host with the same worker count, and the number is in this milestone's Readme. Every rule in the build is listed with its owner; a rule with none, or one kept for a milestone that ended, goes; and the result is proved by an incremental rebuild after a header, a manifest and a configuration change, by two concurrent invocations, and by the built artifacts' contents compared with the clean build's.

### The images go to the machine and the stick

The last act of the milestone is the deployment: the stick rebuilt with recovery and the installer, booted on the notebook and shown to work; the notebook's images updated through the update procedure milestone 49 wrote, under the hardware rule, since the installation was proved in QEMU in milestone 49 and on the Lenovo in milestone 50, and installed again only if the installer changed after milestone 50; the demo checked on the machine.

## Phases

### Phase 1: the build

Product rules out of `suites.mk`; the purge and the parse-time writes made an explicit step; the root Makefile's duplicated declarations and stale comments removed. Proof: `make -n` and an inspection target run with a changed fingerprint and shown to delete nothing, a flag change within one second rebuilt, a parallel build, an incremental rebuild after a header, a manifest and a configuration change, two concurrent invocations, and a check that no recipe under `tests/` names a product; the clean build and the full set come at the close.

### Phase 2: the split

`syscall.c` divided along its existing arms, one commit per extraction. Proof: the suites, `make check-sources`, `guestcheck` and `tests/run selfbuild` unchanged; a script that shows every function body identical before and after is supporting evidence, since headers, macros and linkage can change around identical bodies.

### Phase 3: the test set and the build

The suite and rule inventories; the runner's budgets and isolation; the merges and removals; the new rows; the tiers; the build proved as decided. Proof: the full set's time within the allowance; every suite in the registry named in the inventory with what it proves; the concurrent workloads that failed in milestone 44's pooled runs repeated under controlled contention and passing; the full set runs at the close.

### Phase 4: the bounds

The classification, with the reasons written into the headers, the scripts and the Makefile. Proof: the inventory file in this milestone's directory, every bound in it with its class and reason, read by Codex against the code as part of the review.

### Phase 5: the inventory

Every Readme read; the claim inventory written; the criticism round on Codex, which is prose checking and not the closing code review, its units fixed in `notes.md` first. Proof: the inventory names an owner for every claim or a fix.

### Phase 6: the pilot

The root Readme, the kernel Readme, one component Readme, `vfs.Readme.md`, and one tool Readme, `krffs/Readme.md`, written by Codex from the inventory into the shapes above and checked here. Proof: Codex reads the four against the shape and the inventory and finds nothing. The owner reads them at the start of the milestone's sitting, before the deployment; a shape the owner rejects stops the sitting, every document that took it is redone, the review and the full set run again, and the sitting is called again.

### Phase 7: the rest

Every component Readme into its shape; the administration prose split as decided; the generated inventories. Proof: `tests/run style`, the generator's output matching the tree, a link check, and the checklist: a file of questions a newcomer asks, each answered by a named place in the documents, worked through here with a clock and given to the owner at the start of the sitting; the time is recorded in `notes.md` and the one-minute claim is kept or dropped by the owner's reading.

### Phase 8: the comments

Codex checks the comments against their code, six files per unit. Correct stale comments, implementation narration and forbidden wording here. Proof: `tests/run style` and a second Codex pass over the files that changed finding nothing.

### Phase 9: the close and the deployment

Once: build from clean; the full set; the whole milestone's diff to Codex in units of six files; every finding fixed; the full set again. Then the deployment, in one sitting of the owner's, prepared in full before the owner is called: an inventory written first, with each image, the stick's recovery and installer, the disk's candidate kernel, fallback, recovery and root, its hash, its destination and the boot that proves it; the owner's reading of the pilot documents and the checklist first; then a second stick, not milestone 50's, rewritten on the host under the hardware rule and booted on the notebook and shown to work; the sectors each update overwrites saved on the host and on that stick; recovery made the saved default; then each of the notebook's changed images updated over SSH, each write approved by name, in milestone 49's order: the recovery partition from the booted stick, the root through `rach2-install --update` from the disk's recovery entry, and kernels last, the candidate through `bootctl kernel` and the fallback through `bootctl fallback` from the committed candidate, since `bootctl kernel` moves the saved default to the fallback; then the four boots, the acceptance with the installed credentials and the demo. The owner's part is the reading, the stick, and the approvals. Then notes, the milestone's Readme with the inventory and its hashes, the index entry, and the report to the owner that everything is done, with milestone 50's list empty.

## Risks

* Moving build rules can break a suite's expectations of where an artifact is; each move runs the suites that name the artifact.
* Keep each extraction free of unrelated changes. The body comparison detects edits within functions; review and tests must also check headers, macros and linkage.
* Check each rewritten claim against its recorded evidence before committing it.
* Include a tool Readme in the pilot to check that the shape fits tools as well as subsystems. 
