# Milestone 27: reading every file

## The goal

One person reads every project-owned document and comment, compares it with the code it describes, and corrects false or poorly written prose. Milestone 26 implemented mechanical checks, but those checks detect banned words and over-wide lines, not a grammatical but false sentence. Manual review found eight such sentences in six documents at the end of that milestone, including an unusable fresh-machine setup procedure. The inventory has 485 entries covering 589 files.

## Run procedure

`worklist.md` lists every entry in order with a checkbox. It was derived from `git ls-files` and therefore defines the project-owned scope. The list includes project Readmes and build scripts under `vendor/` and all seven Makefiles; comments occupy one third of the root Makefile. Entries are ordered by their value to a reader, so an interruption leaves the most useful work complete.

Process one entry at a time. Read the complete file, including the code associated with its comments. Correct the prose, mark the entry `[x]`, and only then begin the next entry.

The checkboxes record the position across conversation compactions. After resuming, open `worklist.md` and continue with its first unchecked entry. Do not infer the position from memory or another source.

Commit after every ten completed entries, using the one-line house style. `git log --oneline` then provides an independent position record.

## What may be used

Read and edit the files directly.

Do not substitute `grep`, `rg`, `find`, `python`, or another survey or summary tool for reading a file. The target defects require a person to read every line and cannot be expressed as a search.

Only two commands are allowed, and neither performs analysis: `git commit` after each batch and `make verify-style` after each commit. The verification command is required because this writing pass spans 511 files and cannot validate itself. Without frequent verification, a broken comment delimiter or over-wide line could remain for hundreds of commits. Running it after each batch limits a defect to ten files. Fix every reported failure before continuing.

## What is being judged

Judge style by `CLAUDE.md`: the register and comment rules, message form, ASCII requirement, em-dash prohibition, and unwrapped Markdown prose. Mechanical register checks already pass. This milestone applies the part that requires human judgement.

Correctness is the primary criterion. Compare every documented contract with the code and every function comment with its function. Check the claim as well as its grammar. Milestone 26 found stale counts, file lists, paths, command lines, target names, and descriptions of behavior.

Watch for two defects already found in this tree: a document claiming complete coverage for a subset check, and a comment describing development reasoning instead of the resulting code.

Rewrite an accurate but unclear comment. Correct a false comment. Delete a comment when names and structure now express its content more clearly, as required by `CLAUDE.md`.

## What is out of scope

Vendored trees and `ports/doom/src` are excluded. `worklist.md` contains no third-party file; any apparently vendored path outside project ownership is absent from it.

`dev-log/` records, including this plan, are excluded because they preserve earlier decisions. `dev-log/Readme.md` states that rewriting them would falsify the record.

Code is excluded. This milestone changes documents and comments. When code is wrong, correct its comment, leave the code unchanged, and record the defect. Milestone 28 changes behavior.

## Done when

The milestone is complete when every `worklist.md` entry is checked, `make verify-style` passes, and `dev-log/27-prose/Readme.md` records the false claims, comments that described reasoning instead of behavior, and code defects deliberately left unchanged.
