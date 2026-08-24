# Milestone 26: the prose, the documentation, and the move to another machine

## The goal

This milestone combines two goals: make the project read as if one careful engineer wrote it, and make a clone usable on another Mac. The original plan contained only the second goal. The first began as one phase, but the measurement below shows that it requires more work.

## What the measurement showed, and what it does not show

The repository vendors musl, busybox, dropbear, tinycc, pdpmake, and the Doom engine, all written by other people. The initial comparison measured phrases per thousand words in our C and header comments and in those upstream sources:

    phrase          upstream/1k   ours/1k   ratio
    carries               0.01      0.59     59x
    rather than           0.06      2.10     35x
    holds                 0.05      0.97     19x
    seam                  0.03      0.29    9.6x
    which is              1.77      0.22      8x
    not a                 0.15      0.76      5x

These numbers came from a line-prefix grep over extracted trees, not from the planned checker. They are preliminary measurements, not a baseline. The Codex review assessed each phrase separately, and the later phases follow those assessments.

The ratios for `carries` and `holds` require action, and two outside readers identified those words without seeing this repository. `rather than` provides a useful repetition signal, although teaching prose compares concepts more often than terse implementation comments, so the upstream rate is not a target. The fourth measured term does not provide a fair result because its count combines an established test-suite name, architectural boundaries, an identifier, and loose figurative prose. The phrases `which is` and `not a` primarily measure sentence complexity and negated contracts, which distinguish explanation from implementation. They remain report-only. Markdown has no comparable upstream documentation corpus, so none of its phrase comparisons is fair.

The old register rule's named vocabulary is uncommon in the shipping tree and common in `dev-log`. Its prohibited hyphenated phrase appears 1 time against 28; `honest` appears 5 against 53; and `caveat` appears 2 against 4.

## What is already done

The committed eighty-column comment work (4814e3e, cf36e31, d2fb84b, e58989e) reduced 1,392 over-width comment lines to 16. Every in-house binary had the same hash before and after the work.

The dev-log index and twenty-six current-contract Readmes were checked for stale contracts (98b625a, f5e9961), but not for voice. The work received no complete independent review. Three attempts failed: one reached a ten-minute limit, and two produced nothing. Only the checklist half was reviewed later (b412783), finding seven defects that were fixed. The twenty-six verdicts therefore depend on one writer's reading, so this milestone reviews them again.

## Phase A: the instrument

Phase A builds `tools/prose_check.py` with the following properties, all of which the first draft described incorrectly.

The checker reads vendored upstream prose from the pinned tarballs with Python's `tarfile`. Extracted directories appear only after a build, so requiring them would prevent the checker from running on a fresh clone.

Both corpora use the lexer already written for `tools/check_comments.py`, which tracks block-comment state and string literals. A line-prefix grep counts a C line beginning with `*` as prose and misses every inline comment. Using one extractor prevents that difference from biasing the comparison.

The checker reports three populations separately: C and header comments; shell, make, and Python comments; and Markdown. Only the first has an upstream counterpart. It reports raw counts and word totals for each upstream project instead of one blended ratio, preventing one project from skewing the baseline.

User-visible string literals are in scope because people read test failure messages, and phase C changes four of them. The checker states this boundary.

The checker has two modes. `--banned` rejects terms that are wrong at any frequency. `--report` prints rates and multiples for a watch list and does not fail until phase E selects an enforced subset. An n-gram mode lists the most repeated three- and four-word sequences absent from the watch list, allowing another habit to be found without waiting for an outside reader to identify it.

A selftest exercises every rule with a temporary tree: a banned word in a comment, the same word in a string literal, the same word in this rule's definition, and a block comment spanning multiple lines.

Done when the selftest passes, the tool runs without extracted vendor directories, and its C-comment output matches a committed fixture computed from the tarballs.

## Phase B: the rule

`CLAUDE.md`'s register rule requires judgement, and it failed across twenty-five milestones. Rewrite it in two explicit parts: what a script decides and what a person decides during review. The Codex review identified where the current draft fails this division.

A script decides literal banned phrases, ASCII, dash forms, phrase rates, and a stored budget. A person decides whether "honest" is applied to code, a reversal is rhetorical, a fragment is written for effect, a metaphor is established, a caveat appears beside the claim it limits, or a term of art was defined when first used.

Three statements need correction. "Objects do not act" cannot serve as a general rule because `CLAUDE.md` says that the kernel probes, shared logic lives in `mk/`, and a file fails the build. The actual rule prohibits assigning intentions to hardware and code. "Banned at any frequency, no exceptions" conflicts with the rule's own list, so the definition, checker data, and fixtures receive named exemptions. The Verification section still says that no checker can judge voice, contradicting the register rule; the header of `tools/textcheck.sh` repeats that statement.

Measurements and the name of an unbuilt tool do not belong in a normative file because this milestone will make them stale. The milestone record contains the measurements. `CLAUDE.md` states the rule and identifies its enforcing check.

The term shown on the fourth measurement row and `gate` require inventories instead of global replacement. The first has four meanings: the host test suite's name, an architectural boundary (`rach2/sys/block/Readme.md:37` calls `block_probe` one), the identifier `SEAM_UNINSTRUMENTED` at `tests/host/pwauth_hosttest.c:378`, and loose prose. The suite becomes "the host tests"; each boundary is named for what it bounds; and the identifier is renamed with its uses. The same method applies to `gate`: the suites are `make verify-style` and `make verify-host`, and the target name is shorter and more precise than an abstraction.

`AGENTS.md` gains two enforceable limits learned during this session: never report a delegated job's state without reading it in the same turn, and limit a review so one reviewer can finish within ten minutes.

Done when every rule in both files appears under a heading that identifies whether a script or person decides it, no rules conflict, and both files obey their own rules.

## Phase C: the banned vocabulary

Fix every banned term in the shipping tree, then apply the phase B inventory of the fourth measured term by category. This includes `tools/hosttest.sh:4`, its message at line 112, and the four failure strings in `pwauth_hosttest.c`.

The four strings are user-visible output, so changing them is a behavioral change. Test and commit them separately from the text-only work so that phase D's binary proof has an unambiguous scope.

`dev-log` records preserve what was decided at the time, so rewriting them would falsify the record. They retain their wording. The first draft missed two consequences. First, a future reader rebuilding context from `dev-log` may imitate its prose, so `dev-log/Readme.md` states that the records are evidence rather than a writing model. Second, milestone 27's unexecuted plan is inside the enforced range and must be corrected with the current documents.

Done when `prose_check.py --banned` reports no violations in the shipping tree, the four output strings have a test, and no synonym reintroduces the same abstraction.

## Phase D: the repetition

The first draft proposed a raw-count ratchet with a target of three times the upstream rate for each source root. Review found defects in both parts, so this phase narrows its proof.

Three times upstream is not useful at these rates. For `carries`, that threshold is 0.03 per thousand words, forcing every root below about 33,000 words to contain zero occurrences. This is equivalent to zero tolerance. The rule therefore states zero tolerance directly for `carries` and `holds` when applied to an acting object. Ordinary phrases remain report-only throughout this milestone.

A raw-count ratchet rewards deletion of useful sentences, penalizes a new file with a legitimate occurrence, allows one root to offset another under a global budget, and can be bypassed with a synonym. Instead, `tools/prose_baseline.txt` records counts and word totals by root, the report prints each rate and its change, and phase E decides which conditions fail the build.

The primary work is rewriting. At 2.10 occurrences per thousand words, `rather than` is visible repetition, but the goal is prose without an identifiable model habit, not a target number. Process one source root per commit in descending order of severity: `rach2/sys`; the rest of the kernel; `utils` and `ish`; `krffs`; `tests`; `tools` and `mk`; then Markdown.

The proof used for the eighty-column work does not apply here. That work compared comment text after stripping markers and spacing, which was correct when words could not change. In this phase, intended word changes make equality impossible, while allowing arbitrary differences proves nothing about meaning. The replacement compares source tokens after stripping all comments, proving that no code moved. It also restores the binary comparison specified by an earlier plan revision. The kernel uses `-g`, so a changed comment line count shifts debug tables and requires normalization. Compare three kernels because the metal and recovery builds are separate. The i686-elf ELF and host Mach-O toolchains require different normalizations; combining them would report false code changes. Before use, test each normalization against two builds with no source change. Compare Doom and artifacts that expand `__FILE__` by behavior instead. These checks do not cover the guest build, which uses a second compiler on the same sources; `tools/check_guest.py` and `make selfbuild-test` cover that path.

No hash proves that revised prose preserves its meaning. For each root, a reviewer other than the writer reads the paired before-and-after comments.

Done when each root's commit has identical normalized artifacts, an identical comment-stripped token stream, and a reviewed before-and-after prose comparison, and the report shows a lower rate for every watched phrase.

## Phase E: wiring the checks into make verify-style

`tools/check_comments.py` remains disconnected after the eighty-column work, with 16 over-width lines left. Fix those lines and add the checker beside `textcheck.sh` and `prose_check.py --banned`. Correct the `textcheck.sh` header, which says that voice is not checked.

Use the phase D measurements to decide and document which checks enforce a rule and which only report. The banned list enforces its rule; ordinary phrases remain reports. Promoting a report to an enforced rule is a deliberate policy change. Do not make that change by editing a baseline in the same commit as the prose that raised it.

Done when `make verify-style` rejects an introduced banned term, an over-width comment, and a raised banned count; passes on the repository; and runs without extracted vendor directories.

## Phase F: the move to another machine

Restore the new-machine acceptance criteria from the first plan revision, which a later rewrite weakened.

* `make doctor` identifies every build tool, classifies each one as required or optional, and provides an installation instruction for a missing tool. Prove the check by hiding a tool and confirming that `doctor` identifies it and its fix.
* The root Readme has a bootstrap procedure from a fresh Mac clone through `make full`, using a script-readable manifest of Homebrew formulae.
* The Ubuntu image used by the GRUB step is pinned by digest.
* A clean clone builds in a scratch directory on this machine. This is a smoke test. Building the same clone on the Mac mini is the arrival proof, and its operator performs that test.
* One page lists what a clone omits: nine credential files in three groups, how and why to transfer each group, the SSH host-key fingerprint, GitHub authentication, and the Doom WAD.
* An independent review checks every claim in the twenty-six Readme verdicts from `f5e9961` against the code, using batches small enough to finish.

Seven facts exist only in this machine's memory directory and would be absent from a clone. Write each fact in the file that owns its rule instead of collecting them on one page:

* Prefer busybox to in-house code when a capability is missing. This owner decision belongs under Ports in `CLAUDE.md`.
* Explain the verification policy's required mutation proof: run the mutation from a script with a restoring trap because a mutation can hang the suite and outlive the command that applied it.
* State that restoring with `cp` does not make `make` rebuild. Several runs therefore tested a stale binary and reported PASS for a mutation that had never been compiled.
* Explain how to preserve an independent review's findings: use `tee` to write them to a file, ask the reviewer to state findings early, and limit the scope to work one reviewer can finish within ten minutes.
* Define how a milestone run stops. It continues until the goal is met or three complete attempts to finish a phase have failed, not merely until a convenient checkpoint.
* Limit a goal prompt to one line under thirty words that names the plan file because the destination truncates longer prompts.
* Record the current commit-message form, which differs from the early history.

Done when a clone of the pushed tree builds `make full` in an empty directory on this machine without undocumented steps, `doctor` fails correctly when a tool is hidden, and the clone records all seven facts in their owning files.

## Phase G: the record and the push

From a clean tree, rerun every check, write `dev-log/26-portable/Readme.md` with the measurements and the fixture excluded from `CLAUDE.md` by phase B, add the milestone to `dev-log/Readme.md`, and push.

## What this leaves out, and what could go wrong

Milestone 27 remains unchanged in substance. Its diagnostic work stays there, and phase C corrects only its prose. Do not edit vendored trees. `dev-log` records before this milestone retain their wording.

A synonym can bypass the fixed watch list, which also cannot detect an unknown habit. Phase A's n-gram mode addresses both limits. A person reviews the list before any part of it can fail a build.

A rewrite can change meaning while every artifact remains identical. No hash detects this phase D risk, so someone other than the writer reads the prose in before-and-after pairs.

Phase D is large and repetitive, which encourages an automated rewrite. Three scripts corrupted comments during the eighty-column work, including two changes that the binary hash did not detect. Read the diff from every automated pass before committing it.
