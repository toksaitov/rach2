# Milestone 26: the prose, the documentation, and the move to another machine

## The goal

Two things at once. The project must read as if one careful engineer wrote it, and it must be possible to clone it onto another Mac and carry on. The second was the whole of this milestone when it was first written. The first was one phase inside it, and the measurement below shows it is the larger job.

## What the measurement showed, and what it does not show

This repository vendors upstream sources written by other people: musl, busybox, dropbear, tinycc, pdpmake, and the Doom engine. Comparing our own comment prose against theirs, per thousand words of C and header comments:

    phrase          upstream/1k   ours/1k   ratio
    carries               0.01      0.59     59x
    rather than           0.06      2.10     35x
    holds                 0.05      0.97     19x
    seam                  0.03      0.29    9.6x
    which is              1.77      0.22      8x
    not a                 0.15      0.76      5x

Those numbers came from a line-prefix grep over extracted trees, which is not what the checker will use, so they are a first look and not a baseline. The Codex review of this plan established what they are worth phrase by phrase, and the distinction governs how each is treated below.

`carries` and `holds` are anomalies worth acting on: the ratios are large, and two outside readers named those exact words without seeing this repository. `rather than` is a fair signal of repetition, though teaching prose does compare things more often than terse implementation comments, so upstream's rate is not the target. `seam` is not fairly measured at all, because the count mixes an established test-suite name, genuine architectural boundaries, an identifier, and loose figurative prose. `which is` and `not a` mostly measure sentence complexity and negated contracts, which is the difference between explaining and implementing, so they are watched and reported, never gated. No phrase has a fair Markdown comparison at all, because there is no upstream documentation corpus to compare against.

The named vocabulary from the old register rule is small in the shipping tree and large in `dev-log`: `load-bearing` 1 against 28, `honest` 5 against 53, `caveat` 2 against 4.

## What is already done

The eighty-column comment work is finished and committed (4814e3e, cf36e31, d2fb84b, e58989e): 1,392 over-width comment lines down to 16, every in-house binary hashing the same before and after.

The dev-log index and the twenty-six current-contract Readmes were checked for stale contracts (98b625a, f5e9961) but not for voice. That sweep also never got an independent review: three attempts died, one on a ten-minute limit and two without producing anything, and only its checklist half was reviewed later (b412783, seven defects found and fixed). The twenty-six verdicts therefore stand on one reading by one writer, and this milestone re-reviews them.

## Phase A: the instrument

`tools/prose_check.py`, with these properties, each of which the first draft of this plan got wrong.

It reads the vendored upstream prose from the pinned tarballs with Python's `tarfile`, not from extracted directories. The extracted trees appear only after a build, so a checker that needs them cannot run on a fresh clone, which is the machine this milestone exists to serve.

It uses one extractor for both corpora, and it is the lexer already written for `tools/check_comments.py`, which tracks block-comment state and string literals. A line-prefix grep counts a C line beginning with `*` as prose and misses every inline comment, so the two corpora must not be extracted differently.

It reports three populations separately and never pools them: C and header comments, shell, make and Python comments, and Markdown. Only the first has an upstream counterpart. It reports raw counts and word totals per upstream project, not one blended ratio, so a single project cannot skew the baseline.

It states whether user-visible string literals are in scope. They are: a person reads a test failure message, and phase C changes four of them.

Two modes. `--banned` fails on terms that are wrong at any frequency. `--report` prints rates and multiples for a watch list and never fails anything until phase E says which parts gate. An n-gram mode lists the most repeated three- and four-word sequences that are not on the watch list, so the next habit is discoverable instead of waiting for another outside reader to name it.

A selftest drives every rule from a temporary tree: a banned word in a comment, the same word in a string literal, the same word inside this rule's own definition, and a block comment spanning lines.

done-when: the selftest passes, the tool runs against a tree with no extracted vendor directories, and its C-comment output matches a committed fixture computed from the tarballs.

## Phase B: the rule

`CLAUDE.md`'s register rule asks for a judgement, and it did not hold across twenty-five milestones. Rewrite it in two explicitly separated parts: what a script decides, and what a person decides in review. The current draft fails this, and the Codex review found the ways.

A script decides: literal banned phrases, ASCII, dash forms, phrase rates, and a stored budget. A person decides: whether "honest" is applied to code, whether a reversal is rhetorical, whether a fragment is written for effect, whether a metaphor is established, whether a caveat sits where it belongs, and whether a term of art was defined when first used.

Three specific corrections. "Objects do not act" is impossible as a general rule of English, and `CLAUDE.md` itself says the kernel probes, the shared logic lives in `mk/`, and a file fails the build; the rule is about giving hardware and code intentions, and must say so. "Banned at any frequency, no exceptions" is contradicted by the rule itself, which prints every banned term, so the definition, the checker's data, and its fixtures are exempt by name. The Verification section still says voice is what no checker can judge, which now contradicts the register rule, and the same sentence appears in the header of `tools/textcheck.sh`.

Measurements and the name of an unbuilt tool do not belong in a normative file, because they go stale the moment this milestone succeeds. They live in the milestone record; `CLAUDE.md` states the rule and names the check that enforces it.

The `seam` and `gate` question is settled here, on an inventory rather than a global replace, because the word is doing four different jobs: the name of the host test suite, a genuine architectural boundary (`rach2/sys/block/Readme.md:37` calls `block_probe` one), the identifier `SEAM_UNINSTRUMENTED` at `tests/host/pwauth_hosttest.c:378`, and loose prose. The suite becomes "the host tests". A boundary is named for what it bounds. The identifier is renamed with its uses. `gate` goes the same way: the suites are `make verify-style` and `make verify-host`, and naming the target is shorter than naming an abstraction.

`AGENTS.md` gains the two working rules this session paid for, stated as limits a person can check rather than as advice: never report the state of a delegated job without a command in the same turn that reads it, and scope a review so one reviewer finishes inside ten minutes.

done-when: every rule in both files sits under a heading that says whether a script or a person decides it, no rule contradicts another, and the files obey themselves.

## Phase C: the banned vocabulary

Fix every banned term in the shipping tree; the counts are small. Then the `seam` inventory from phase B, category by category, including `tools/hosttest.sh:4`, its message at line 112, and the four failure strings in `pwauth_hosttest.c`.

Those four strings are user-visible output, so changing them is a behavioural change and not a text-only one. They are tested, and they are committed apart from the text-only work so the binary proof in phase D is not muddied by them.

`dev-log` records are what was decided when, and rewriting them would falsify the record. They keep their wording. Two consequences the first draft missed: a future reader rebuilding context from `dev-log` will imitate what it reads unless told not to, so `dev-log/Readme.md` says plainly that the records are evidence of what happened and not a model for how to write; and milestone 27's plan, which is not yet executed, is in the enforced range and is corrected with the rest.

done-when: `prose_check.py --banned` reports nothing in the shipping tree, the four output strings have a test, and the same term has not reappeared under a synonym.

## Phase D: the repetition

The first draft proposed a ratchet on raw counts with a target of three times the upstream rate per source root. Both halves were wrong and the review showed why, so this phase is narrower and more honest about what it can prove.

Three times upstream is not a criterion at these rates. At `carries`, three times upstream is 0.03 per thousand words, so every root under about 33,000 words would have to contain zero, which is a zero-tolerance rule wearing a ratio. Zero tolerance is therefore stated where it is meant, for `carries` and `holds` used of an object acting, and the ordinary phrases stay report-only through this milestone.

A raw-count ratchet rewards deleting a useful sentence, punishes adding a file that legitimately needs one occurrence, lets one root pay for another when the budget is global, and is defeated by a synonym. What replaces it: `tools/prose_baseline.txt` records counts and word totals per root, the report prints the rate and the direction of travel, and phase E decides what actually fails a build.

The work is the rewriting. `rather than` at 2.10 per thousand words is a habit visible to any reader, and the goal is prose that no longer announces its author, not a number. One source root per commit, worst first: `rach2/sys`, the rest of the kernel, `utils` and `ish`, `krffs`, `tests`, `tools` and `mk`, then the Markdown.

The proof from the eighty-column work does not transfer, and this is the most important correction in this phase. That work compared comment text with markers and spacing stripped, which was exactly right when the words could not change. Here the words change on purpose, so an equality check would reject every intended edit, and a check that permits differences proves nothing about meaning. What replaces it: the source with all comments stripped is compared token by token, which proves no code moved while the prose changes freely, and the binary comparison is restored in the form the previous revision of this plan specified, since that detail was lost and is not obvious. That form is: the kernel is built with `-g`, so debug tables shift when a comment line count changes and normalization is required; three kernels, not one, because the metal and recovery builds are separate; two toolchains mean two normalizations, i686-elf ELF and host Mach-O, and conflating them reports a code change where there is none; each normalization is checked against itself by building twice with no source change before it is trusted; Doom and any artifact expanding `__FILE__` are compared behaviourally instead; and the guest build is a second compiler over the same sources that none of it covers, which `tools/check_guest.py` and `make selfbuild-test` are for.

No hash proves that prose still means what it meant. The paired before-and-after comment text is read, per root, by a reviewer who is not the writer.

done-when: each root's commit shows identical normalized artifacts, an identical comment-stripped token stream, and a reviewed before-and-after of its prose; and the report shows every watched phrase moving down.

## Phase E: wiring the checks into make verify-style

`tools/check_comments.py` from the eighty-column work is still unwired, at 16 remaining over-width lines. Fix those, wire it in, and wire in `prose_check.py --banned` beside `textcheck.sh`. Correct the header of `textcheck.sh`, which tells the reader voice is not checked here.

What gates and what only reports is decided here, with the phase D numbers in hand, and written down with its reason. The banned list gates. The ordinary phrases report. Anything promoted from report to gate later is a policy change, made deliberately, not by editing a baseline file in the same commit as the prose that raised it.

done-when: `make verify-style` fails on an introduced banned term, on an over-width comment line, and on a raised banned count; passes on the tree; and runs on a clone with no extracted vendor directories.

## Phase F: the move to another machine

The acceptance criteria here are the ones the first revision of this plan set, which the rewrite had quietly weakened.

* `make doctor` names every tool the build needs, marks each required or optional correctly, and says what to install when one is missing. It is proved by a deliberate negative test: hide a tool, confirm `doctor` says so and names the fix.
* A bootstrap section in the root Readme taking a fresh Mac from a clone to `make full`, with the Homebrew formulae in a manifest a script reads.
* The Ubuntu image used for the GRUB step pinned by digest.
* A clean-clone build into a scratch directory on this machine. This is a smoke test, not the arrival proof: the arrival proof is the same clone built on the Mac mini, and it belongs to whoever runs it there.
* One page listing what a clone does not carry: the nine credential files in three groups, how each is transferred and what carrying it buys, the SSH host-key fingerprint, the GitHub authentication, and the Doom WAD.
* An independent review of the twenty-six Readme verdicts from `f5e9961`, split into batches small enough that each finishes, checking every claim against the code it describes.

Seven facts are held only in this machine's memory directory and a clone loses them. Each is written into the file that owns the rule, not collected in one page:

* Prefer busybox to in-house code when a capability is missing. The owner decided this; it belongs in `CLAUDE.md` under Ports.
* How a mutation is proved to fail, which the verification policy demands and never explains: drive it from a script with a restoring trap, because a mutation can hang the suite and outlive the command that applied it.
* That a `cp` restore does not make `make` rebuild, so several runs re-tested a stale binary and a mutation reported PASS when it had never been compiled.
* How to run an independent review so its findings survive: `tee` it to a file, tell the reviewer to state findings early, and scope it to what one reviewer finishes inside ten minutes.
* How a milestone run is conducted: it does not stop at a comfortable checkpoint; the only stops are the goal being met or a phase failing three honest attempts.
* That a goal prompt is one short line naming the plan file, under thirty words, because a longer one is truncated where it is sent.
* The commit message shape this history uses now, which has drifted from the early entries.

done-when: a clone of the pushed tree into an empty directory builds `make full` on this machine with no undocumented step, `doctor` fails correctly when a tool is hidden, and a reader of that clone finds all seven facts in the files that own them.

## Phase G: the record and the push

Re-run everything from a clean tree, write `dev-log/26-portable/Readme.md` with the measurements and the fixture that phase B kept out of `CLAUDE.md`, add the milestone to `dev-log/Readme.md`, push.

## What this leaves out, and what could go wrong

Milestone 27 is untouched in substance: the diagnostics work stays there, and only its prose is corrected in phase C. The vendored trees are never edited. `dev-log` records before this milestone keep their wording.

The watch list is a fixed list, and a fixed list is defeated by a synonym and blind to the next habit. That is what the n-gram mode in phase A is for, and it is the reason the list is reviewed by a person before any part of it fails a build.

A rewrite can change meaning while every artifact stays identical. This is the real hazard of phase D, and no hash addresses it, which is why the prose is read in pairs by someone other than the writer.

Phase D is large and boring, and the temptation is a clever script. Three scripts corrupted comments during the eighty-column work, two of them in ways the binary hash could not see. Every automated pass is read as a diff before it is committed.
