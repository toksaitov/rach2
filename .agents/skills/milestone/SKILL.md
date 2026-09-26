---
name: milestone
description: Work through a milestone of dev-log/ and close it. Use when a task runs from a dev-log plan.md, before a milestone's closing runs, Codex review or machine sitting, and before calling a milestone or plan done. Not for a small change outside a milestone.
---

# Milestone

A milestone runs from its `plan.md` under `dev-log/`, and is built and checked in batches, as the Batches section of `AGENTS.md` says. This skill has the details a milestone adds.

## While a batch is built

* Run `tests/run style host` at the end of the batch before committing a change to a kernel header or a host test source.
* A Readme whose contract a commit changes is rewritten in that commit.
* A problem found along the way that the milestone did not set out to fix goes into `dev-log/50-shortcomings/plan.md` the same day, with its severity, how to reproduce it, the contract it breaks, what it depends on, and the test that will show it fixed. The milestone that found it fixes it, and the entry says so. Nothing is reported at the end as found and unfixed.
* A step that costs the owner time or a device is checked before it starts: a build from clean, a full set, a Codex round, a hardware sitting, or any run a person waits for. Read what would stop it: `dev-log/50-shortcomings/plan.md`, the milestone's notes and the code the step depends on. Where a form is known to work, use it. A step whose conditions were not checked is not started.

## The checks

* The build from clean empties `build/` of everything but `build/vendor/`, and removes the vendor outputs as well only when a port or its toolchain changed.
* The style suite runs `prosecheck`, the mechanical half of the register rule; the review covers its contextual half.
* The Codex review takes the whole batch's diff in units grouped by contract, each of at most six files and sized to finish in ten minutes. Each unit gets the shared interface changes, may read what its diff depends on, and states its most serious findings first. An expired review is incomplete, not clean.
* Fix every finding, or reject it in the milestone's `notes.md` with the source that shows it wrong.

## The machine sitting

* Only a milestone whose plan has a machine phase works on the machine; the others test their work in QEMU and do not touch it. A kernel installed through `bootctl` over SSH when the machine happens to be on is permitted, never required.
* The machine phases are gathered so that the owner is needed for one sitting. Every step is prepared before the owner is called, the steps run one after another, and the sitting ends with the owner free.

## Done

* Before a milestone or a plan is called done, or nearly done, audit its whole scope: every phase of its plan, every entry of the shortcomings list, and every record, read for what is still owed. Report the list of open items the audit gives; nothing is called done that the audit has not shown done.
* The milestone's Readme records what was accepted: the commit, the build options, the image hashes, the test command and its result, the hardware arrangement, and the versions of the testing host's compilers, analysers, formatter and QEMU.
