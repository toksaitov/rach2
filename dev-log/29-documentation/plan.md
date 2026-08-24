# Milestone 29: documentation

## The goal

Read and correct every documentation file this project owns. The current documents are not accepted as evidence for their own claims. Each current-contract document is checked against the implementation, build rules, manifests, tests, and commands it describes, then rewritten in the prose required by `CLAUDE.md`.

The historical records under `dev-log/` receive a different treatment. They are proofread for grammar, clarity, consistency, and the prohibited model register. Their technical claims, decisions, numbers, commands, chronology, and conclusions are preserved even when the current tree differs. This milestone must not turn a historical record into a description of the present system.

The work is deliberately serial. One primary agent reads and edits one file at a time. No part of the reading, fact checking, rewriting, or final review is delegated to another agent or model.

## The inventory

`worklist.md` is the complete ordered inventory. It was made from the 123 tracked Markdown files present when this plan was written: 28 current-contract documents outside `dev-log/`, and 95 historical documents under `dev-log/`. The five live files in `dev-log/29-documentation/` are control files for the run and are not part of that count. `current.md` is the bounded checkpoint for the file in progress. `finish.md` expands the last whole-project stages into small, durable entries.

At the start of the run, and after any later change to the set of tracked Markdown files, compare `git ls-files '*.md'` with `worklist.md`. Exclude `dev-log/29-documentation/` from that comparison. A newly added project document is added to the worklist before the run continues. An absent path is investigated rather than marked complete.

The current-contract inventory includes `AGENTS.md`, `CLAUDE.md`, the root and component Readmes, the subsystem Readmes, and the six project-written integration Readmes under `vendor/`. Those vendor files are ours and are in scope:

* `vendor/busybox/Readme.md`
* `vendor/busybox/patches/Readme.md`
* `vendor/dropbear/Readme.md`
* `vendor/musl/Readme.md`
* `vendor/pdpmake/Readme.md`
* `vendor/tcc/Readme.md`

Upstream documentation inside the pinned archives or their extracted directories is not in scope and is not proofread. Do not add an upstream `README`, manual, changelog, license, or generated document to the worklist merely because a build extracted it under `vendor/`. Fact-check our integration Readmes against our build scripts, configs, patches, staged artifacts, and the relevant source when needed. Do not edit an extracted vendor tree or a pinned archive.

The tracked `ports/doom/Readme.md` is project documentation and is in scope. This milestone does not edit source comments, Makefile comments, program messages, legal texts, manifests, scripts, code, or configuration files. They may be read as evidence. A defect found outside documentation is recorded in `notes.md` and left for separate work.

## Which evidence decides a claim

For a current-contract document, use evidence in this order:

1. The code, Makefiles, scripts, manifests, configs, and patches that implement the claim.
2. Tests and checkers that exercise the claim, including their exact assertions and prerequisites.
3. A direct, non-destructive command or build that demonstrates the claim.
4. Recent commits when the implementation does not explain why a contract has its present shape.
5. Another current document only as a pointer to the evidence above.

Do not settle a disputed fact by choosing the most polished document or the most recent milestone record. Documentation can repeat the same mistake, and `dev-log/` describes what was believed and done at the time.

Counts, paths, target names, dependency lists, command lines, image contents, modes, users, addresses, limits, and test coverage are all claims. Trace them to their source. If a command is safe and reasonably bounded, run it. If running it would alter hardware, publish data, or require authority outside this milestone, use the code and tests and say in the final record that it was not run.

For `AGENTS.md` and `CLAUDE.md`, preserve the owner's rules. Correct prose and factual references, but do not change a policy decision merely to make it resemble current code. Record a contradiction in `notes.md` if resolving it would change the rule rather than the description.

For a `dev-log/` file, do not perform the factual procedure above. Read the file whole and proofread only its prose. Preserve its meaning. Do not update an old path, command, count, hardware result, source location, model name, plan, or conclusion to match the current tree. Do not add hindsight. A sentence may be recast to remove bad prose, but its historical assertion must remain the same.

## The required prose

Apply the writing rules in `CLAUDE.md` to every edited file, including the historical records now explicitly placed in scope by the owner for proofreading.

Write short, exact sentences. State the component, what it does, and why the fact matters. Remove slogans, filler, private shorthand, false contrasts, dramatic fragments, repeated conclusions, model mannerisms, and explanations of the writing process. Define a term when a reader first needs it. Put a qualification beside the claim it limits.

Current Readmes describe the current contract. Rewrite stale sections instead of appending corrections or a development history. Delete duplicated material when the owning document can state it once and another document can link to it. Keep detail that teaches the implementation or prevents a real mistake. Do not shorten a document by removing its necessary contract.

Historical records remain records. Improve their sentences without converting them into current reference documentation.

ASCII, Markdown bullets, headings, code fences, links, and commands follow `CLAUDE.md`. Do not wrap Markdown prose to a fixed column.

## The arrival check

Before editing the first listed document:

1. Read `AGENTS.md`, `CLAUDE.md`, this plan, `worklist.md`, and `notes.md` in full.
2. Run `make doctor` and require every required row to pass.
3. Require a clean working tree and record `git log --oneline -12` in the session context.
4. Run `make full`, `make verify-style`, `make verify-host`, and `make test`. A failure is diagnosed before documentation work begins. It is not attributed to prose changes that have not happened.
5. Run `make help` and keep its target list available while checking build instructions.
6. Confirm that the worklist still equals the tracked Markdown inventory, apart from this milestone's control files.

The plan was prepared after `make doctor` passed on the new machine. The goal run repeats it because the goal, not the planning conversation, owns its preconditions.

## Recovery after every compaction

Compaction loses conversational detail, but it does not invalidate repository evidence already recorded as a navigation aid. Recovery must restore enough context to finish the current file. It must not restart a repository-wide audit.

Before another edit after compaction:

1. Read `AGENTS.md`, `CLAUDE.md`, this plan, `worklist.md`, `notes.md`, and `current.md`.
2. Run `git status --short --branch`, read the milestone's commits with `git log --oneline`, and identify the first unchecked worklist entry.
3. Read the complete current document and its complete uncommitted diff. Inspect every other uncommitted path before continuing.
4. Use `current.md` to locate the evidence already examined and the unresolved checks. The checkpoint is not factual authority. Reopen the implementation or test for each claim that will be edited, but read only the function, rule, configuration block, or test case that decides it.
5. Read the root `Makefile` or root `Readme.md` only when the current document makes a project-wide build, target, image, or ownership claim. Locate the relevant rule first and read its complete recipe and prerequisites. Do not reread either root file from beginning to end merely because compaction occurred.
6. Do not reread an entire subsystem as a recovery ritual. Start with the edited sentences and open questions in `current.md`. Expand to callers or adjacent code only when the narrow evidence does not decide the claim.

Keep recovery output bounded. A single command should print no more than 200 lines or about 20,000 bytes. Split a larger file around named functions or sections. Generated summaries and `current.md` choose where to look; only the repository decides the fact.

The first unchecked entry is the only source of the next position. A checked entry is complete. The current document is always read whole after compaction, but its completed evidence audit is not repeated without a concrete unresolved claim.

If two compactions occur on the same file without a commit, the next recovery must not repeat any broad read already named in `current.md`. Recheck only the unresolved claims, finish the document, verify it, and commit it. This rule prevents recovery itself from consuming the next context window.

### Recovery during the final stages

The final-stage procedure begins after every individual document, every subsystem pass, the historical collection pass, and the first five whole-project checks are complete. During this procedure, `finish.md` is the detailed progress ledger. Its first unchecked entry is the only next position.

After a compaction in the final-stage procedure:

1. Read `AGENTS.md`, `CLAUDE.md`, `finish.md`, and `current.md` in full. In this plan, read from `## The whole-project pass` through the end. Do not reread the completed inventory, arrival procedure, individual-file history, full `worklist.md`, or full `notes.md` as a ritual.
2. Run `git status --short --branch` and read the recent milestone commits. Use `rg -n '^\* \[ \]' dev-log/29-documentation/worklist.md dev-log/29-documentation/finish.md` to confirm the two levels of progress without printing the completed entries.
3. Inspect every uncommitted path. If a documentation file is active, read that complete file and its complete uncommitted diff. Reopen only the narrow evidence named in `current.md`.
4. Resume the first unchecked `finish.md` entry at the exact candidate or diff-output line recorded in `current.md`. Do not rerun a completed search, recreate a completed candidate list, or restart a completed file.

Limit a diff page to 80 output lines. Record the next output line in `current.md` before reading another page. A changed diff invalidates that file's cursor, so restart that file at output line 1 after editing it. It does not invalidate any earlier checked file.

## The current-file checkpoint

`current.md` carries only the state needed to resume one file. Update it before a long evidence pass, after each bounded evidence batch, before a likely compaction, and before verification. Keep it short and replace obsolete details instead of appending a diary.

It records:

* the first unchecked file and whether its original and current forms were read whole;
* the active `finish.md` entry, candidate path, or diff-output line during the final stages;
* the changed sections and the claims they now make;
* repository paths and function or target names that decide those claims;
* facts already checked, open questions, and the exact next action;
* commands already run for the current file and their result;
* the intended commit boundary.

`current.md` is a map, not evidence. After a compaction, re-open the narrow source location before relying on a checked fact in an edit. Once the file is committed, replace the checkpoint with the next file's starting state. Do not copy its temporary detail into the final milestone record.

## The one-file procedure

For each entry in `worklist.md`:

1. Read the entire document without using a search result or summary as a substitute.
2. Set `current.md` to this file and list its material claims before opening a large body of evidence.
3. For a current-contract document, inspect the implementation and tests that decide each material claim. Read enough surrounding code to understand the contract rather than matching one token. Work in bounded batches and update `current.md` after each batch.
4. Record cross-document contradictions, non-document defects, facts needed by later passes, and decisions that may be lost after compaction in `notes.md`.
5. Rewrite the document in place. Do not edit a second documentation file while the first is in progress.
6. Check every local link, path, command, target, count, version, mode, and named artifact in the edited file.
7. Review the file's diff by itself. Confirm that it changes documentation only and preserves any historical assertion that is out of factual scope.
8. Run `python3 tools/prose_check.py --banned <file>` and a direct ASCII check on the file. Run the repository text checks for current documentation. Fix every result before proceeding.
9. Add any lasting result to `notes.md`, mark the worklist entry `[x]`, update `current.md` to say that the file is ready to commit, and commit the file with its control-file changes.

Do not mark a file complete because it needed no edit until it has been read under this procedure. Do not start the next file while the present entry is unchecked.

Search tools may locate the implementation of a claim after the document has been read. They may not replace reading the document or the relevant code. Generated summaries, word-frequency reports, and automated rewrites may identify questions; they do not decide prose or facts.

Do not maximize source coverage in one context window. The useful unit is a finished documentation file, not the number of implementation lines read. Once every changed sentence has direct evidence and the required checks pass, commit the file instead of reopening settled parts of the subsystem.

## Checkpoints and commits

Keep commits small enough to review as prose:

* Commit a substantial current Readme by itself. Closely related short Readmes may share a commit, but never more than five completed current-contract files.
* Commit historical proofreading in ordered batches of no more than ten completed files.
* Run `make verify-style` before every commit. Run `make verify-host` at the end of each current-document subsystem and before the final record.
* Inspect `git diff --check` and the staged diff before every commit.
* Use the commit style in `CLAUDE.md`. Do not amend unrelated work, attribute the change to a model, or push.

`worklist.md` is committed with the boxes completed in the same commit as the documents they describe. `notes.md` is committed when a batch adds information to it. The working tree is clean at each checkpoint.

During the final stages, update `finish.md` immediately after each completed entry. Commit manifest-only progress after one large diff file or at most five small diff files. If a documentation edit is required, return to the one-file procedure and commit that file before continuing the manifest. Run `make verify-style` and inspect the staged diff before every such commit.

## The current-document pass

Process the 28 current-contract documents in the order in `worklist.md`. The order establishes the policy and repository model first, then the build and test machinery, kernel and subsystems, userland, and external integrations. A later document may expose an error in a completed one. Record it in `notes.md`; do not interrupt the current file. Return to the earlier file during the subsystem pass, one file at a time.

For each component Readme, inspect its owning directory before editing it. For subsystem Readmes, inspect the public headers, implementation files, callers, and probes. For tool and test Readmes, inspect the Make targets and scripts that invoke them. For vendor integration Readmes, inspect our tarball version, hash check, build script, config, patches, staged paths, and consumers.

## The historical proofreading pass

After every current-contract document is complete, process the 95 pre-existing `dev-log/` files in worklist order. Read each file whole. Correct prose and style only.

Do not use current source to revise a historical statement. Do not normalize old milestone plans into the current process. Do not remove evidence that a plan was wrong or that a review changed it. Preserve quotations, prompt text, test output, commands, commit ids, and recorded results unless the surrounding prose alone needs correction and the quoted bytes remain unchanged.

The early foundation prompts are separate files and each is a separate entry. Their instructions are historical content. Proofread explanatory prose around literal prompts, but do not rewrite the prompt that was actually used when doing so would change the record.

## The subsystem passes

Once every individual file is checked, make a higher-level pass over the current documentation in these groups:

1. Policy, onboarding, build, portability, and tooling.
2. Kernel boot, memory, processes, signals, TLS, and entropy.
3. File systems, descriptors, synthetic files, pipes, polling, terminals, and block storage.
4. Network drivers, configuration, protocols, sockets, and network utilities.
5. Images, KRFFS host tools, accounts, init, shell, utilities, and self-build.
6. musl, BusyBox, Dropbear, TinyCC, pdpmake, and Doom integration.
7. Tests, host tests, specialized harnesses, and the claims made about verification.

For each group, compare the documents with one another and with the implementation. Check terminology, boundaries, prerequisites, repeated facts, and ownership. Use `notes.md` to revisit contradictions found during the serial pass. If an edit is needed, return to the one-file procedure and finish one document before changing another. Mark a subsystem pass complete only when its documents agree on the current contract or state their different scopes clearly.

Then make a prose-only collection pass over `dev-log/`. Look for repeated model mannerisms and inconsistent naming introduced by this milestone's own edits. Do not compare the history with current code and do not change historical substance.

## The whole-project pass

After the subsystem passes, inspect the documentation as one set.

* Confirm that the root `Readme.md` gives a correct route into every component and does not duplicate subsystem detail that belongs elsewhere.
* Confirm that `AGENTS.md`, `CLAUDE.md`, the root Readme, component Readmes, and `make help` agree on build, verification, and git procedure.
* Check all relative Markdown links and every referenced tracked path.
* Compare all named Make targets with the Makefiles and `make help`.
* Search repeated numeric and version claims, then trace each one to its authority.
* Check that one term names one concept across the repository, and that each component has one owning Readme. Use the bounded terminology entries in `finish.md`. A search prints candidate paths only; copy that queue into `current.md`, then inspect one candidate file at a time. Do not rerun the search after the queue is recorded.
* Read the complete documentation diff in repository order using the fixed milestone base and file manifest in `finish.md`. Read one file at a time in 80-line output pages. `current.md` records the next output line. Fix one file at a time under the one-file procedure, then restart that file's diff review from line 1.
* Confirm the changed-path scope with the commands in `finish.md`. No upstream document, code file, config, script, manifest, generated file, or legal text may have changed.

The whole-project pass includes the root Readme again. Its first review establishes context; this pass judges whether the completed component documents make its overview true.

## Verification and completion

Before recording the milestone:

1. Require every file and every higher-level pass in `worklist.md` to be checked.
2. Re-run the tracked Markdown inventory comparison.
3. Run the banned-prose check explicitly over every edited `dev-log/` file, because the ordinary text checks exclude historical records.
4. Run `make full`, `make verify-style`, `make verify-host`, and `make test` from the completed documentation tree.
5. Run any specialized target used as direct evidence for a corrected claim. List it in the final record.
6. Run `python3 tools/check_devlog.py --selftest` and `python3 tools/check_devlog.py`.
7. Inspect `git diff --check`, the complete milestone diff, and the path list from the fixed base recorded in `finish.md`. Only in-scope Markdown files and this milestone's control files may differ.
8. Confirm that the worktree is clean after the final commit.

Then write `dev-log/29-documentation/Readme.md`. Record the scope, the false current claims corrected, the recurring prose faults removed, any documentation deliberately left unchanged, non-document defects recorded for later work, and every verification command actually run. Do not turn the record into another copy of the component documentation.

Add `29-documentation` to `dev-log/Readme.md` only when that final record exists. The record and the index entry are one commit, so the index never states that unfinished work is complete. Run `tools/check_devlog.py` on that commit.

The milestone is done when the final record and index are committed, all worklist entries are checked, every required verification passes, and the working tree is clean. Do not push.
