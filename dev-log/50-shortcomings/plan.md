# Milestone 50: the shortcomings

## The goal

Every problem found during milestones 45 to 49 that those milestones did
not set out to fix, fixed. The list below is written as the work goes: an
entry the day the problem is found, with the file and line, what is wrong,
and what a fix needs, and nothing is deferred past this milestone. The
manuals and the audit follow it, so that what they describe is the
finished system and not one with known holes. The milestone is complete
when every entry is marked fixed with its proof, and the close below has
run.

## What the tree settles before this plan

This section is filled in as entries arrive. Each is a fact with a file
and line, in the form the other plans use.

## Entries

An entry has a date, the milestone that found it, the file and line, one
sentence on what is wrong, one on what a fix needs, and, once done, the
commit and the proof. The list is empty until milestone 45 starts.

## Phases

### Phase 1: the fixes

The entries in the order that touches the fewest files last: kernel
first, then tools and tests, each with the smallest suite that covers it.

### Phase 2: the close

Proof, once and in this order: build from clean; the full set; a boot on
the Lenovo and the SSH capstone; everything that comes up fixed; the whole
milestone's diff to Codex in units of six files, most serious first; every
finding fixed; the full set again. Then notes, the milestone's Readme and
the index entry.

## Risks

* A list that grows faster than it is worked is the sign that a
  milestone's scope was wrong, and the owner is told when it passes
  twenty entries rather than at the end.
