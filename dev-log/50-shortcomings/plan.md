# Milestone 50: the shortcomings

## The goal

Fix every previously known problem and every problem recorded during milestones 45 through 49. The list below is written as the work goes: an entry the day the problem is found, and nothing is deferred past this milestone. The manuals and the audit follow it, so that what they describe is the finished system and not one with known defects; an entry made during those two reopens the technical work before the document that describes it is accepted. The milestone is complete when every entry is marked fixed with its proof and the close below has run.

## What the tree settles before this plan

* The stick behind the SuperSpeed hub stopped answering and its removal panicked the kernel once; the record leaves the reset-and-removal sequence with stale completions open (`dev-log/43-machine/notes.md:291`). A healthy stick pulled is not the same test.
* Milestone 44 needed four full runs to get one clean, and records the failures as pooled execution (`dev-log/44-krffsmount/notes.md:412`). A successful rerun does not explain the earlier failure. Diagnose the kernel, harness and scheduling before closing the entry.

## Entries

An entry has: a date; the milestone that found it; its severity, with a loss of data or a panic first; how to reproduce it; the contract it breaks, with file and line; what it depends on; the test that will show it fixed; and, once done, the commit and the proof. The two above are the first entries, numbered 1 and 2, and the list is worked in order of severity and dependency, not of the code it touches.

1. The stopped stick: reproduce the never-answering reset and the removal with completions still pending, in QEMU with a stick that stops answering, and fix what the kernel does with the stale completions.
2. The pooled runs: find why a suite that passes alone fails in the pool, which is milestone 52's scheduling work brought forward as far as the cause requires.

## Phases

### Phase 1: the fixes

The entries in order of severity, each with the smallest suite that covers it and a mutation for any new check.

### Phase 2: the close

Proof, once and in this order: build from clean; the full set; everything that comes up fixed; the whole milestone's diff to Codex in units of six files, most serious first; every finding fixed; the full set again; then the Lenovo and the SSH capstone. Record the results in `notes.md` and the milestone's Readme. Add the index entry.

## Risks

* Notify the owner when a severe problem is added. One entry that loses data matters more than twenty that do not. 
