---
name: milestone
description: Work through a milestone of dev-log/ and close it. Use when a task runs from a dev-log plan.md, before a milestone's closing runs, Codex review or machine sitting, and before calling a milestone or plan done. Not for a small change outside a milestone.
---

# Milestone

A milestone runs from its `plan.md` under `dev-log/`. Its phases are issues on the board and their tasks are sub-issues, each landed by a pull request, as the Work section of `AGENTS.md` says. This skill has the details a milestone adds.

## While a task is built

* When a task changes a kernel header or a host test source, run `tests/run style host` once at the end of the task, before committing it.
* A Readme whose contract a commit changes is rewritten in that commit.
* A problem found along the way that the milestone did not set out to fix becomes an issue with the label `bug` the same day, with its severity, how to reproduce it, the contract it breaks, what it depends on, and the test that will show it fixed. The issue is a sub-issue of the milestone's phase, so the milestone that found it fixes it, and the pull request that fixes it closes it. Nothing is reported at the end as found and unfixed. `dev-log/50-shortcomings/plan.md` is the record of the problems found before 2026-10-13, and takes no new entries.
* A step that costs the owner time or a device is checked before it starts: a build from clean, a full set, a Codex round, a hardware sitting, or any run a person waits for. Read what would stop it: `dev-log/50-shortcomings/plan.md`, the milestone's notes and the code the step depends on. Where a form is known to work, use it. A step whose conditions were not checked is not started.

## The close

* At the end of each phase, Codex reviews the phase's whole diff from the owner's account, and each finding it keeps becomes an issue. The owner closes a milestone after a build from clean and a full set on its last commit.
* The build from clean empties `build/` of everything but `build/vendor/`, and removes the vendor outputs as well only when a port or its toolchain changed.
* The style suite runs `prosecheck`, the mechanical half of the register rule; the review covers its contextual half.
* A phase's review takes its diff in units grouped by contract, each of at most six files and sized to finish in ten minutes, one unit at a time. Each unit gets the shared interface changes, may read what its diff depends on, and states its most serious findings first. An expired review is incomplete, not clean.
* Fix every finding, or reject it in the milestone's `notes.md` with the source that shows it wrong.

## The machine sitting

* Only a milestone whose plan has a machine phase works on the machine; the others test their work in QEMU and do not touch it. A kernel installed through `bootctl` over SSH when the machine happens to be on is permitted, never required.
* The machine phases are gathered so that the owner is needed for one sitting. Every step is prepared before the owner is called, the steps run one after another, and the sitting ends with the owner free.

## Done

* Before a milestone or a plan is called done, or nearly done, audit its whole scope: every phase of its plan, every open issue with the label `bug`, and every record, read for what is still owed. Report the list of open items the audit gives; nothing is called done that the audit has not shown done.
* The milestone's Readme records what was accepted: the commit, the build options, the image hashes, the test command and its result, the hardware arrangement, and the versions of the testing host's compilers, analysers, formatter and QEMU.
