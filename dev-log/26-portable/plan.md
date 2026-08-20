# Milestone 26: the documentation, the comments, and the move to another machine

## The goal

The work moves to a second machine. A Mac mini M4 replaces the laptop as the machine this project is built on, because the milestones now run for days and a machine that stays powered and reachable is worth more than a portable one. Milestone 27 runs there.

For that to work the repository has to describe itself accurately, and it does not. `AGENTS.md` tells a new reader that the last entry in `dev-log/Readme.md` is where the tree stands; that entry is milestone 24, and two milestones have shipped since. Anyone following the instruction lands two milestones in the past without knowing it.

So this milestone is three passes and no new features: the documentation, the comments, and the arrangements a second machine needs. It ends with the tree pushed, and the first thing milestone 27 does is prove the new machine can build and test what it cloned.

## What was measured before this plan

Both machines are Apple Silicon macOS, so nothing here bridges an architecture. This laptop is `arm64` on macOS 26.6.2 and the Mac mini is an M4. `qemu-system-i386` here is a native arm64 build, so no Intel emulation is involved in running the OS. The one cross-architecture dependency is Docker: three scripts run a Debian container with `--platform linux/amd64` (`tools/mkiso.sh:154`, `tools/mkgrubcore.sh:53`, `tools/mkdisk_grub.sh:390`).

The vendored tarballs are committed and `.git` is 25 MB, so the source needs no downloads. The build does: Homebrew formulae, a Docker image pull, and the packages that image installs with `apt-get` at build time.

`make doctor` passes here with every tool present. Its classification is wrong in a way that matters on a fresh machine: it marks `cppcheck`, `shellcheck`, `clang-format`, `perl`, Docker and `ssh-keygen` optional, and `tools/check.sh` needs the first four, `krffs/gen_demo_key.sh` exits fatally without `ssh-keygen`, and `make sealed-test` cannot build its disk without Docker. It also omits `i686-elf-ar`, `i686-elf-ranlib` and `i686-elf-nm`, which the Dropbear and TinyCC builds invoke by name, and `expect` and `timeout`, which two harnesses require outright.

The in-house tree has 991 comment lines longer than 80 columns: 847 in `rach2`, 80 in `tests`, 33 in `utils`, 28 in `krffs`, 3 in `ish`. `CLAUDE.md` requires 80 and `tools/check.sh` does not check it. `tools/` and `mk/` have over-width comments too and were not in that count.

The kernel is built with `-g` (`rach2/Makefile:118`, `:188`), so the host kernel carries DWARF line tables and an absolute compilation directory. Milestone 21 made staged images reproducible, not the host kernel binary. Any proof that a comment change did not alter code has to compare loadable content, not the whole ELF.

Nine generated files are git-ignored, not five: two private keys, their two public halves, two staged authorized-keys files, two plaintext passwords, and `shadow`.

## Phase A: the index, and the facts the next machine needs

`AGENTS.md` sends a new reader to `dev-log/Readme.md` and says its last entry is where the tree stands. Make that true: add milestone 25 in the form the other entries take.

Then record the hardware session that happened after milestone 25 was written and is nowhere in the tree. A prose account is not enough, because the next person to touch that drive needs to know which drive and which artifact. The milestone 25 record takes the non-secret results the sealing checklist already asks for: the artifact's SHA-256 and byte count, the readback hash and that it matched, how the target was identified and that the adapter reports no serial, the address the router assigned and the MAC it matched, the ordinary system's SSH host-key fingerprint, the first `bootctl status`, and where the SSD physically is. Passwords and private keys are not written into the repository; phase E says what carrying them buys and what it does not.

`dev-log/25-sealed/Readme.md` says the hardware is untouched. True when written, false now. It is corrected rather than appended to.

Done when a reader following `AGENTS.md` from a clean clone can say where the tree stands, which artifact is on the SSD, how to recognise that drive, and how to authenticate the machine it becomes.

## Phase B: the current-contract Readmes

The tree tracks 51 Readmes. Twenty-five are milestone records under `dev-log/` and are history, not rewritten. Twenty-six describe current contracts and are swept: each read against the code it describes, every false statement corrected in place, a verdict recorded for every file including the accurate ones. Four are easy to miss because their names carry a prefix (`rach2/sys/fs/synthfs.Readme.md`, `rach2/sys/lib/krandom.Readme.md`, `rach2/sys/proc/signal.Readme.md`, `rach2/sys/proc/tls.Readme.md`), so the sweep enumerates from `git ls-files`.

Two are already known wrong. `vendor/busybox/Readme.md:82` says this kernel offers neither raw nor packet sockets; packet sockets arrived in milestone 25. `rach2/Readme.md:61` says `make subset-test` boots every combination of the four device-skip tokens; it boots six lines, being none, each alone, and all four together.

Tool and script prose states contracts too and is in scope. `tools/runtests.py`'s header still describes an older scoring scheme.

One document is corrected to a policy the owner has now decided. `dev-log/25-sealed/checklist.md` requires the target drive's model and serial from `diskutil info`, and the adapter in use reports `Generic` with no serial, so that check cannot run and the procedure stops.

The replacement is a confirmation rather than a comparison, and `CLAUDE.md` now carries it as project policy so it reaches whoever or whatever runs the procedure next, on any machine. Before any destructive write, the operator lists the candidate drive and everything the host can say about it (node, size in bytes, bus, internal or external, whole disk, and the partition table it currently carries), presents that to the owner, and writes only on an explicit approval naming that node. An approval does not carry forward to the next run, and one given on another machine does not carry at all, because the node number is a property of the host and the moment rather than of the drive. The mechanical guards in the block stay as they are, since they cost nothing: not the boot disk, external, USB, whole disk, a size within bounds, and the artifact's hash checked before and after.

The checklist stops naming a specific node in its example, since a copied example is the thing most likely to be pasted unread.

Worth stating in the checklist alongside it, because it is the question the old wording invited: nothing in Rach2 is bound to a particular drive. The kernel probes AHCI for whatever is there and finds its root by scanning partition entries for the KRFFS magic, so the image boots from any drive carrying the layout. The serial was never an operating requirement; it was a guard on the host's `dd`, and a person confirming the drive in front of them is a stronger guard than a string comparison against a label.

`dev-log/24-metal/baseline.md` gains the four facts about the Lenovo's Windows side that exist only in an untracked report: the SATA link power management values that must stay at zero and why, Fast Startup being enabled so a shutdown is hybrid, BitLocker never having been enabled, and the BIOS version and date with back-flashing disabled. The report is then deleted.

The files are independent, so this phase spreads across parallel readers.

Done when every one of the twenty-six has a verdict, every correction is committed, the checklist states a decided policy, the four facts are in the baseline, and the report is gone.

## Phase C: the comments

Bring the in-house comments to the 80-column rule the project already states, and add the check that keeps them there.

Scope is named exactly, because "the in-house tree" is ambiguous and the ambiguity would decide what gets skipped. Covered: `rach2`, `utils`, `ish`, `krffs`, `tests`, `tools`, `mk`, and `ports/doom`, which `Readme.md:29` calls code this project owns. Not covered: `vendor/`, whose trees are upstream, and `build/`. C and header comments in both syntaxes, and shell and Python comments in the scripts. The checker is lexer-aware, so a comment delimiter inside a string literal is not mistaken for a comment.

The safety proof has to survive the fact that the kernel is built with `-g`. Adding or removing a comment line moves DWARF line tables and declaration-line attributes, so the whole ELF changes even when no instruction does, and the compilation directory is absolute so two clones in different paths differ anyway. The comparison is therefore between copies stripped of debug information, built by the same compiler with the same flags in the same working directory, before and after.

It rests on no in-house source using `__LINE__` or `__FILE__`, which is true today outside Doom. A pass that introduced either would invalidate the proof it depends on.

The artifact list is everything the pass can reach, not one binary. Three kernels, since the metal and recovery builds compile the same sources into their own trees (`Makefile:258`, `Makefile:271`). The utilities and `ish`. Every executable under `build/tests` and `build/tests/musl` whose sources were edited. The KRFFS host tools. And Doom.

Two toolchains mean two normalizations, and conflating them would report a code change where there is none. The cross-built ELF artifacts take `i686-elf-strip --strip-debug`, which is enough here: those outputs carry no build-id and no timestamp note, and the link adds nothing but the script. The KRFFS tools are built by the host compiler and are arm64 Mach-O, which `i686-elf-strip` cannot read at all; they also carry an `LC_UUID` the linker derives from output content and an ad-hoc signature, so stripping after the link does not necessarily normalize what the link already computed. They are compared either by linking the proof copies with the UUID and ad-hoc signing suppressed and then running the native `strip -S`, or by extracting and hashing the loadable sections alone.

Before either normalization is trusted it is checked against itself: build twice with no source change at all and require the hashes to match. A procedure that cannot produce equal hashes for identical input proves nothing about unequal ones, and discovering that at the end of a 991-line pass is the expensive way to learn it.

Two artifacts stay behavioural rather than byte-compared, for stated reasons. Doom expands `__FILE__` and `__LINE__` (`ports/doom/include/assert.h:12`, `ports/doom/src/z_zone.h:69`), so a reflow there changes its binary legitimately. The host tests built with the sanitizers embed source locations for the same reason, so they are judged by running them.

The guest build is a second compiler over the same sources and is not covered by any of that. `tools/check_guest.py` compares lists and recipes, not output, and `make selfbuild-test` hashes two builds of the same tree, so it cannot see a TinyCC-specific difference introduced by the pass. So a TinyCC kernel hash is taken before the pass begins and compared after.

Beyond width, since the pass reads every comment anyway: a comment describing behaviour the last three milestones changed is corrected, and a comment narrating inside a function body, which `CLAUDE.md` allows only for a hardware or ABI fact that cannot be named away, is removed or moved into the block above the function.

Done when no covered comment line exceeds 80 columns, the gate fails if one does, the stripped kernel and every other affected in-house binary hash the same before and after, the TinyCC kernel hash is unchanged, and Doom still runs.

## Phase D: the toolchain check, and what a fresh Mac actually needs

`make doctor` is corrected to match what the verification sequence really requires, and the repository gains the bootstrap the Readme currently assumes.

Reclassify as required, because a gate target cannot run without them: `cppcheck`, `shellcheck`, `clang-format`, `perl`, Docker and `ssh-keygen`. Add checks that do not exist: `i686-elf-ar`, `i686-elf-ranlib`, `i686-elf-nm`, `expect`, `ssh`, `timeout` or `gtimeout`, `strings`, `gzip`, a SHA-256 program, and `git`. Assert GNU Make rather than any make. For Docker, check that the daemon answers and that an `amd64` container actually runs, since the command being present proves neither. Record the ordinary shell utilities the scripts use as macOS base or Xcode-provided, so the list is complete rather than partly unexamined.

The bootstrap is written down as a sequence a person can follow on a machine with nothing on it: Xcode Command Line Tools, Homebrew, the formulae and casks (from a tracked manifest rather than prose, so it cannot drift from doctor), start Docker and confirm it answers, confirm an amd64 container runs, confirm free space, then clone and run doctor. The space figure is measured, not guessed: the sealed artifact alone is 630 MiB and `sealed-test` keeps a working copy and an extracted root beside it.

Two external facts go in the same place. The build is not offline: it pulls a Docker image and that image installs packages with `apt-get` at build time, from a floating tag. Pin the image by digest so a rebuild is not at the mercy of an upstream retag. And pushing needs GitHub write authentication, which is per-machine and not part of a clone.

On this machine the phase proves what it can: a clone into a scratch directory, `make doctor`, `make`, `make full`, and doctor failing when a required tool is made unavailable. That is a smoke test and the plan says so, because a clone here still sits on this machine's Homebrew, Xcode, Docker and caches, and `make full` does not run QEMU at all.

Done when doctor's classification matches what the gate needs, the bootstrap manifest exists and agrees with doctor, the Ubuntu image is pinned, and the scratch clone builds `make full` while a deliberately absent required tool makes doctor fail.

## Phase E: the arrival, and what a clone does not carry

The real proof that the move worked cannot be produced on this machine. It is an arrival check on the Mac mini, from a fresh clone: `make doctor`, `make full`, `make verify-style`, `make verify-host`, the full test list, `make selfbuild-test`, and `make sealed-test`. Milestone 26 cannot contain it, so it is milestone 27's first act and a precondition to everything after it, written into that plan rather than assumed.

What a clone does not carry, and what that costs:

The nine generated credential files, in three groups. Two open the existing machine and cannot be regenerated: `krffs/rach2_demo_ed25519` and `krffs/rach2_test_ed25519`. Three reproduce an image with the same credentials: `krffs/rach2_demo_password`, `krffs/rach2_test_password`, `krffs/shadow`. Four are derived public copies that the generator rebuilds: the two `.pub` files and the two authorized-keys files. Copying the first five brings the rest with them. One trap: `gen_demo_key.sh` decides whether to mint from the presence of the authorized-keys file, not the private key, so copying a private key without its staged public half causes the generator to replace the key that was just copied.

The transfer is a convenience rather than a precondition, and it became one during planning. The artifact now on the SSD carries the owner's own public key beside the project's demo key, so the Mac mini reaches the laptop with the key already in its `~/.ssh`. Milestone 27's hardware tests in phases F and G therefore need none of the project's private material. Copying the five files still buys the ability to rebuild an image with identical credentials and to log in as the demo accounts, which is worth having; without them the new machine mints its own and the images it builds differ from the one on the drive until phase K rewrites it.

One mechanism already exists and needs only its documentation. `krffs/gen_demo_key.sh` now composes each authorized-keys file from the generated public half plus an optional `krffs/authorized_keys.extra`, git-ignored, so a person can add their own public key to images they build without carrying the project's private key anywhere. It recomposes rather than appends, so rebuilding never doubles a line and deleting the file removes the addition, and it refuses a file holding a private key or a line that is not a public key. That was built and tested during planning; this phase gives it its entry in `krffs/Readme.md` and commits it with that entry, as the project requires of a change that alters a contract.

Genuinely required by hand: the Dropbear host-key fingerprint of the ordinary system, so the new client can authenticate the existing machine rather than accepting whatever answers; GitHub write authentication; and the Doom WAD, which is operator-supplied and git-ignored, without which Doom does not run but nothing else is affected.

Everything else that is ignored is regenerable from tracked inputs: `build/`, the extracted vendor trees, the vendor stamps, the installed musl, and `compile_commands.json`.

### The knowledge that is not in the tree

Each assistant keeps notes outside the repository, in a directory keyed by the project's path on that machine, so they do not survive a move even if the whole directory were copied to a clone at a different path. Most of what they hold is a summary of a milestone whose record is in `dev-log/`, and those can be lost without cost. Seven items are not written down anywhere in the tree, and they are carried in:

* The standing preference for busybox over in-house code, decided by the owner: when a capability is missing, look for an applet before writing a program, and keep `utils/` small. Write in-house only when busybox's version cannot work here or cannot be given the privilege it needs. The salt and locking objections were weighed and set aside, and are not to be re-argued. Goes in `CLAUDE.md` beside the ports policy.
* How a mutation is proved to fail, which the verification policy demands but never explains. Drive it from a script that copies the file, sets a restoring `trap` on `EXIT INT TERM` before mutating, applies a hard time cap, and always restores; verify the mutation actually applied and check the tree afterwards. A mutation that leaks a blocked signal mask makes the test unkillable, the command times out, and a trailing restore never runs, which once left a source file mutated across several commits. Goes in `tests/Readme.md`.
* That a `cp` restore does not make `make` rebuild. Several runs re-tested a stale binary, so a mutation reported PASS and a restored tree reported FAIL. `touch` the file after restoring and confirm the compiler named it. Same place.
* How to run the independent review so its findings survive: pipe it through `tee` into a file and tell the reviewer to state each finding's file, line and reasoning early rather than in a trailing summary, because the output is repeatedly found holding only its closing lines and nothing recovers them afterwards. Goes in `AGENTS.md`, which is what the other assistant reads first.
* How a milestone run is conducted: it does not stop at a comfortable checkpoint. The only stops are the goal being met, or a phase that cannot be met after three honest attempts, reported with evidence. "The next part is large" and "fresh context would be better" are the work, not stop conditions. Same place.
* That a goal prompt is one short line naming the plan file, under thirty words. A longer one is silently truncated at the first paragraph break by the editor plugin, and renders an overlay that hides the whole chat for the duration of the run, so the owner cannot watch the work or tell that it has died. Same place.
* The commit message shape the history actually uses now, which has drifted from the early entries: one capitalized verb-led sentence saying what the change does and why, ending with a period, no body, no attribution. `CLAUDE.md` says scoped units and no attribution but not the shape; the shape goes beside it.

Two more belong in `CLAUDE.md` because they are about how the work is written rather than what it does. The register rule there covers invented jargon; it does not cover the opposite failure, which is explaining an ordinary term to a reader who has known it for twenty years. And chat replies are held to the same standard as committed prose, which the file says, but the standard is "no decoding required", not "simpler".

Done when `dev-log/26-portable/Readme.md` lists each of these with which group it belongs to and says plainly which are required and which are a convenience, and milestone 27's plan carries the arrival check as the act before its first phase.

## Phase F: the gate, the record, and the push

Run the whole gate from a clean tree: `make verify-style`, `make verify-host`, the full suite, `make selfbuild-test`, `make sealed-test`. Write the record in `dev-log/26-portable/Readme.md` and add this milestone to `dev-log/Readme.md` so the index is current the moment the work ends.

Push, since the next machine gets everything by clone and work committed locally does not travel.

Done when every gate passes from a clean tree, the record is written, the index names this milestone last, and the remote holds it.

## Risks, in expected order

Phase C is a very large mechanical diff across eight directories. The stripped-binary comparison is what makes it safe, and it is worth running after each scoped commit rather than once at the end.

Phase D can pass on this machine because this machine has everything. The deliberate-absence test and the tracked bootstrap manifest are what carry the weight; the arrival check in milestone 27 is what actually settles it.

Phase E is the one whose omissions are invisible until the new machine needs something. The host-key fingerprint, the WAD and the push credential are checked by milestone 27's arrival check rather than trusted to have been read here.

Phase B can turn into a rewrite. The rule is correction in place with a verdict per file.

Phase A is short and is the one a reader depends on most.

## What this milestone deliberately leaves out

* Every code change milestone 27 owns. Nothing here touches the kernel or the userland except comments.
* Installing anything on the Mac mini. This milestone produces the list and the sequence; someone at that machine follows it.
* CI. The clone build is run by hand.
* Any change to the disk layout, the boot menu, the two levels of disk authority, or the sealing procedure other than the checklist decision in phase B.

## What must not be broken

* The build. Phase C changes comments only, proved on stripped binaries and on the TinyCC output as well as the host one.
* The dev-log records as history. They are corrected only where they state something false about the present, as phase A does for milestone 25's hardware claim.
* The guest build's equality with the host lists, held by `tools/check_guest.py`.
* Access to the machine currently on the SSD, until milestone 27 rewrites it.
* ASCII only, and the voice rules, in every line of prose this milestone touches, which is most of them.
