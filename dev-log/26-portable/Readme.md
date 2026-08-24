# Milestone 26: the prose, the documentation, and the move to another machine

This milestone had two tasks: make a clone usable on another machine and remove model-specific habits from the prose. The prose work was larger and occupies most of this record.

## What set it off

An outside reader on a public forum published a tool that sends this model's output through another model to produce ordinary English. The thread identified specific habits. Two named words, `carries` and `holds`, were this project's largest measured anomalies, although nobody here had noticed them. Since milestone 6, `CLAUDE.md` had prohibited a model-specific register. Across twenty-five milestones, the named terms remained in the tree, and nobody had measured the repeated ordinary phrases that produced the same signature.

## The measurement

The repository contains five vendored upstream projects as pinned tarballs and the Doom sources, totaling about 700,000 words of comments written outside this project. These files provide an in-tree control corpus from the same language and domain. The comparison used occurrences per thousand words of C and header comments:

    phrase          upstream/1k   before   after
    carries               0.02     0.65     0.00
    holds                 0.06     1.11     0.24
    rather than           0.07     2.48     0.30
    , not                 0.37     1.06     2.77
    which is              0.25     2.03     2.03
    not a                 0.23     0.92     1.22

`tools/prose_check.py` computes the rates directly from the tarballs. A fresh clone has no extracted build directories until a build runs, so depending on them would prevent the check from running on the destination machine. Both corpora use the lexer already implemented by `tools/check_comments.py`. The comparison therefore uses one parsing method for both sources.

## What was found, and what it cost to find

Review found a defect in the first version of each part.

The checker initially measured formatting as well as prose. A phrase split across a line break was omitted, while each standalone `*` counted as a word: 18,275 project occurrences and 34,890 upstream. Since the corpora wrap comments differently, layout biased every rate. The corrected checker normalizes each comment before counting.

`--banned` claimed to inspect string literals because users read diagnostics, but it inspected only C strings. Shell, make, and Python strings were not checked.

A targeted run could pass without selecting a file. `prose_check.py --banned tools` returned OK because arguments were compared as exact filenames against a prepared list. A directory, typo, or path outside the roots selected zero files and still succeeded.

Six terms left the mechanical list because each has a legitimate use in this project. `the trap` refers to the shell builtin in twelve scripts. `bench` identifies a test bench in `utils/dhcpmsg.c`. `gate` describes the timer and speaker hardware gates at port 0x61. `surface` appears in attack surface and public surface. `era` is ordinary English, and `sight` appears in the idiom "anywhere in sight". Only two of the seven proposed shorthand nouns remained prohibited. The original rule had not been checked against its domain.

## The rename

The more common prohibited term required the largest single change: 94 uses with four distinct meanings, which ruled out global replacement. The host test suite became "host test". A function substituted by a test became a stub or interposed call. A ring buffer's physical boundary became "the wrap". A driver-selection location became "where that choice is made". The same work changed a macro and four test diagnostics. Since those changes affect behavior rather than comments alone, they were committed separately and identified by token comparison.

## The trade that was not a win

Recasting `rather than` redistributed contrast constructions without reducing their total rate. Before the edit, `rather than` appeared 2.48 times per thousand words, `, not` 1.06, and `instead of` 0.31, for 3.85 combined. Afterward, the rates were 0.30, 2.77, and 0.71, for 3.78 combined. The largest single phrase fell from 34 times the upstream rate to 7.5, reducing one recognizable habit while leaving overall contrast density nearly unchanged. Both replacement forms are now measured. A lower total requires deciding which contrasts are necessary in each sentence, work outside this milestone.

## What now fails a build

Step 8 of `make verify-style` runs three checks. `tools/textcheck.sh` retains its checks for non-ASCII text and simulated em-dashes. `tools/check_comments.py` enforces the 80-column comment limit; it was written during earlier comment work but not connected until the tree had no failures. `tools/prose_check.py --banned` checks vocabulary that can be decided mechanically. Mutation tests confirmed that each check rejects an introduced defect.

Repetition remains a report rather than a build failure. `prose_check.py --report` prints measurements for human review, while `tools/prose_baseline.txt` records the watched-phrase baseline and its change. A numeric budget would impose an unapproved rule. At these rates, a per-root ratio effectively permits no occurrence, while a decreasing raw-count limit rewards deletion of useful sentences.

## A regression this milestone caused, and caught

Earlier comment work wrapped an 85-column `host-deps:` line in two host tests. The parser in `tools/hosttest.sh` stops at the next `*`, so it discarded every path after the line break. `uimage_hosttest` no longer linked, while `kstd_hosttest` lost a dependency and continued passing accidentally. The 42-artifact hash comparison did not cover host tests and could not detect the regression.

The correction follows `tests/host/Readme.md`: repeat the `host-deps:` prefix. `hosttest.sh` now refuses a wrapped declaration. `tests/musl/build.sh` used similar parsing with `head -1`, which would discard a repeated declaration. No current test has two declarations, so that correction is preventive.

## Proving a text change is a text change

The earlier 80-column work compared comment text after removing markers and spacing. That method was correct when words could not change, but it cannot verify this work because the intended edits change words. Exact equality would reject every correction, while permitting arbitrary differences would prove nothing.

The replacement strips all comments and compares source tokens. It permits prose edits while proving that code tokens remain unchanged, and it reports deliberate string changes separately. This check found a marker-spacing edit that matched line-leading pointer dereferences and changed `*wq = NULL;` to `* wq = NULL;` in twenty places. The code still compiled, passed clang-format, and passed both check suites. Only token comparison detected it.

## The move to another machine

`make doctor` classified `cppcheck`, `shellcheck`, `clang-format`, and `perl` as optional, although `tools/check.sh` refuses to start without them and `CLAUDE.md` requires that suite for every change. They now form a separate required group. If any one is hidden, `doctor` names it and exits non-zero.

`tools/mac-formulae.txt` provides one readable list of the formulae and casks. `tools/bootstrap_mac.sh` installs from that list, skips installed items, and accepts `--dry-run` to show the list first. The three GRUB steps use an Ubuntu image pinned by digest in `tools/ubuntu-image.txt`. A tag can move, and using another `grub-pc` build would introduce an unchosen change.

`dev-log/26-portable/arrival.md` lists what a clone omits: ten git-ignored credential files in three groups, the machine's own SSH host key that does not exist until the first boot, the Doom WAD, the build tree, and GitHub authentication. The files that own each rule now state seven working facts that had existed only in this machine's memory.

A clean clone in an empty directory supplied the proof. `make doctor` passed; `make` and `make full` built every capstone from the tarballs without fetching anything; and `make verify-style` and `make verify-host` passed there.

## What the documentation review found

The prose passes were reviewed during the work. A final comparison of the documents with the code found eight errors. Two were substantial.

The fresh-Mac sequence did not work. It installed Homebrew and then invoked a script that aborts unless `brew` is already on PATH. A fresh installation requires a separate `shellenv` step, so the published sequence stopped at its first consequential command. The Readme now runs `shellenv`, and `bootstrap_mac.sh` checks both Homebrew installation locations before failing.

`make doctor` omitted `i686-elf-ar`, `i686-elf-ranlib`, and `i686-elf-nm`, although the build invokes all three. A partial cross toolchain could therefore pass the toolchain check and fail during `make`.

The other errors were stale counts and lists: eleven credential files rather than ten, with each generator stopping at a sentinel instead of filling gaps, so a partial transfer remained broken through a build; eighty-seven guest kernel objects rather than seventy-one; twenty-nine utilities rather than twenty-five, including four that were built but never exercised despite a claim that each was checked; eight of the thirty-five host tests missing from a purportedly complete list; a tooling reference naming seven of the ten inputs read by the GRUB builder; an executable path invalidated by the out-of-tree build; and a block device described as a four-call interface although it has two operations and three data fields.

One finding was partly wrong. The review identified three undocumented GRUB inputs, but the same file documented two of them elsewhere. Only one was missing.

## What is deferred, and why

Reducing the contrast density described above requires sentence-by-sentence judgement, so it is deferred instead of receiving another rule-based pass.

The phrases `which is` and `not a` remain at 8.2 and 5.2 times their upstream rates. The plan review found that they primarily measure sentence complexity and negated contracts, which distinguish explanatory prose from implementation. Teaching prose legitimately uses more of both. The phrases remain monitored without further action.

The `dev-log` records before this milestone retain their wording because rewriting them would falsify the record of earlier decisions. `dev-log/Readme.md` now identifies them as evidence, not as a writing model.

Long-term reproducibility of the GRUB disk remains deferred. Two builds of one tree on one day are byte-identical, and the base image is pinned by digest. The build still runs `apt-get install` against a mutable archive, so that archive determines the GRUB and mtools versions. The document no longer claims more. Full reproducibility would require pinned package versions or an archive snapshot.

The arrival check on the destination machine also remains deferred. The local clean-clone build is a smoke test. Building the same clone on the Mac mini supplies the proof, and that action belongs to its operator.
