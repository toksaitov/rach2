# Milestone 26: the prose, the documentation, and the move to another machine

Two jobs in one milestone. The project had to become something a second machine could pick up from a clone, and its prose had to stop announcing which model wrote it. The second turned out to be the larger job, and it is the one this record is mostly about.

## What set it off

An outside reader, on a public forum, published a tool whose only purpose is to run this model's output through a second model to turn it into English. The complaints in that thread were specific, and two of the words they named, `carries` and `holds`, turned out to be this project's two largest measured anomalies. Nobody here had noticed. `CLAUDE.md` had banned a register in prose since milestone 6, and across twenty-five milestones that ban had held for none of it: the terms it named by hand were still in the tree, and the habit it did not name, repeating one ordinary phrase until it becomes a signature, had never been looked for.

## The measurement

This repository vendors five upstream projects as pinned tarballs, plus the Doom sources, together about 700,000 words of comment prose written by people with no connection to this project. That is a control corpus in the same language and the same domain, already in the tree. Measured against it, per thousand words of C and header comments:

    phrase          upstream/1k   before   after
    carries               0.02     0.65     0.00
    holds                 0.06     1.11     0.24
    rather than           0.07     2.48     0.30
    , not                 0.37     1.06     2.77
    which is              0.25     2.03     2.03
    not a                 0.23     0.92     1.22

`tools/prose_check.py` computes this. It reads the tarballs directly rather than the extracted build directories, because extraction happens during a build and a fresh clone has only the tarballs; a check that needed the extracted trees could not run on the machine this milestone exists to serve. Both corpora go through one lexer, the one `tools/check_comments.py` already had, so a difference in the numbers is a difference in the writing and not in how the two sides were read.

## What was found, and what it cost to find

The first version of every part of this was wrong in a way a review caught.

The checker measured formatting as much as prose: a phrase broken across a line break went uncounted, and a lone `*` counted as a word, 18,275 of them on our side and 34,890 upstream. Since the two corpora wrap comments differently, every rate was biased by layout. Prose is now normalized one comment at a time before anything is counted.

`--banned` documented that it read string literals, because a person reads a test failure message, and read them only in C. Every shell, make and Python string went unscanned.

A targeted run reported success on nothing: `prose_check.py --banned tools` returned OK because arguments were matched as exact filenames against a prebuilt list, so a directory, a typo, or a path outside the roots all selected zero files and passed.

Three terms had to leave the mechanical list because they have legitimate uses here, and three more followed for the same reason. `the trap` is the shell builtin twelve scripts discuss. `bench` is a test bench in `utils/dhcpmsg.c`. `gate` is hardware, the timer and speaker gates at port 0x61. `surface` is an attack surface and a public surface. `era` is ordinary English. `sight` is the idiom "anywhere in sight". Of the seven invented shorthand nouns the rule listed, only `seam` and `timebox` survived. The rule had been written without checking it against the domain it governs.

## The rename

`seam` was the largest single job: 94 uses, and they were not all the same thing. A global replace was forbidden for that reason, and the inventory found four distinct meanings. The name of the host test suite became "host test". A function a test substitutes to reach a moment inside the code became a stub, or an interposed call. A ring buffer's physical wrap point became "the wrap". The place a driver is chosen became "where that choice is made". A macro and four test failure messages changed with them, and those are behavioural changes rather than text, so they were committed apart and the token comparison surfaced each one.

## The trade that was not a win

Recasting `rather than` moved its weight rather than removing it. Before: `rather than` 2.48 per thousand words, `, not` 1.06, `instead of` 0.31, together 3.85. After: 0.30, 2.77, 0.71, together 3.78. The worst single phrase fell from 34 times the upstream rate to 7.5, which is a real improvement in how much the prose announces itself, but the density of contrast constructions is what it was. Both replacement forms are now watched and recorded for that reason. Reducing the total means deciding, sentence by sentence, which contrasts are worth drawing at all, and that is work this milestone did not do.

## What now fails a build

`make verify-style` step 8 runs three checks. `tools/textcheck.sh` for non-ASCII and faked em-dashes, as before. `tools/check_comments.py` for the 80-column comment rule, written during the earlier comment work and left unwired until the tree reached zero, because a check that fails on the day it lands teaches everyone to ignore it. And `tools/prose_check.py --banned` for the vocabulary a script can decide. Each was mutation-tested through the gate and each fails on an introduced defect.

Repetition does not fail a build. `prose_check.py --report` measures it and a person reads that; `tools/prose_baseline.txt` records where the watched phrases stand and prints the change since. Making it a budget would turn a report into a rule nobody agreed to, and the review of this milestone showed why: a per-root ratio at these rates is zero tolerance wearing a ratio, and a raw-count ratchet rewards deleting a useful sentence.

## A regression this milestone caused, and caught

The earlier comment work rewrapped an 85-column `host-deps:` line in two host tests. `tools/hosttest.sh` parses that line with a regex that stops at the next `*`, so every path after the break was dropped: `uimage_hosttest` stopped linking, and `kstd_hosttest` lost a dependency and kept passing by luck. The 42-artifact hash proof could not see it, because host tests are not in that list.

It was fixed the way `tests/host/Readme.md` already documented, by repeating the `host-deps:` prefix, and `hosttest.sh` now refuses a wrapped declaration outright. `tests/musl/build.sh` had the same parsing with `head -1`, which would drop a repeated declaration entirely; no test has two yet, so that fix is preventive.

## Proving a text change is a text change

The comment-text comparison used for the earlier 80-column work does not transfer here, and this is the correction that mattered most. That work compared comment text with markers and spacing stripped, which was exactly right when the words could not change. Here the words change on purpose, so an equality check would reject every intended edit and a check that permits differences proves nothing.

What replaced it compares the source with all comments stripped, token by token. It proves no code moved while the prose changes freely, and it surfaces deliberate string changes rather than hiding them. It earned its place: a marker-spacing repair matched pointer dereferences at the start of a line and turned `*wq = NULL;` into `* wq = NULL;` in twenty places. That still compiles, still passes clang-format, and both gates stayed green. Only the token comparison saw it.

## The move to another machine

`make doctor` was telling a reader on a fresh machine that `cppcheck`, `shellcheck`, `clang-format` and `perl` were optional, when `tools/check.sh` refuses to start without them and `CLAUDE.md` requires that suite on every change. They are now a required group of their own, and hiding one makes `doctor` name it and exit non-zero.

`tools/mac-formulae.txt` lists the formulae and casks in one place a person can read; `tools/bootstrap_mac.sh` installs from it, skipping what is present, with `--dry-run` to see the list first. The Ubuntu image the three GRUB steps run in is pinned by digest in `tools/ubuntu-image.txt`, because a tag moves and a GRUB built from a different `grub-pc` is a change nobody chose.

`dev-log/26-portable/arrival.md` says what a clone does not carry: ten git-ignored credential files in three groups, the machine's own SSH host key which does not exist until it boots, the Doom WAD, the build tree, and the GitHub authentication. Seven working facts that lived only in this machine's memory are now in the files that own each rule.

The proof is a clean clone into an empty directory: `make doctor` passes, `make` and `make full` build every capstone from the tarballs with nothing fetched, and `make verify-style` and `make verify-host` both pass there.

## What the documentation review found

The prose passes were reviewed as they went. The documents themselves were reviewed last, against the code rather than for how they read, and that pass found eight things wrong. Two mattered.

The fresh-Mac sequence did not work. It installed Homebrew and then called a script that aborts unless `brew` is already on PATH, which after a fresh install it is not: the installer leaves that to its own `shellenv` step. Anyone following the printed sequence would have stopped at the first command that mattered. The Readme now runs `shellenv`, and `bootstrap_mac.sh` looks in both places Homebrew installs to before giving up.

`make doctor` did not check `i686-elf-ar`, `i686-elf-ranlib` or `i686-elf-nm`, all of which the build invokes, so a partial cross toolchain could pass the toolchain check and fail during `make`.

The rest were counts and lists that had drifted: eleven credential files rather than ten, and each generator stops on a sentinel rather than filling gaps, so a partial transfer stays broken through a build; eighty-seven guest kernel objects rather than seventy-one and twenty-nine utilities rather than twenty-five, with four of them built and never exercised where the document claimed a check for each; eight of the thirty-five host tests missing from the list that claims to be all of them; a tooling reference naming seven of the ten inputs the GRUB builder reads; a path to an executable that the out-of-tree build moved; and a block device described as a four-call interface when it has two operations and three data fields.

One finding was partly wrong and checking it mattered: the review said three GRUB inputs were undocumented, and two of the three were documented elsewhere in the same file. Only one was actually missing.

## What is deferred, and why

The contrast density above, which needs sentence-by-sentence judgement rather than another pass of rules.

`which is` at 8.2 times upstream and `not a` at 5.2. The review of the plan established that these mostly measure sentence complexity and negated contracts, which is the difference between explaining and implementing; teaching prose legitimately does more of both. They are watched and not acted on.

The `dev-log` records before this milestone keep their wording, because rewriting them would falsify the record of what was decided when. `dev-log/Readme.md` now tells a reader that they are evidence and not a model to write from.

Reproducibility of the GRUB disk across time. Two builds of one tree on one day are byte-identical, and the base image is now pinned by digest, but the build still runs `apt-get install` against a mutable archive, so the GRUB and mtools inside it are whatever that archive serves. The document no longer claims more than that; pinning package versions or an archive snapshot is the work that would.

The arrival check on the destination machine itself. The clean-clone build here is a smoke test; the proof is the same clone built on the Mac mini, and it belongs to whoever runs it there.
