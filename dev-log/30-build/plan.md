# Milestone 30: the build

## The goal

Rebuild the build system and the support tooling around a small public interface, one scripting language plus C, and a smaller required tool set, without changing what the operating system itself does. At the end, a third party builds and boots Rach2 with the i686-elf cross toolchain, GNU Make, a host C compiler, a POSIX shell with the base POSIX utilities (including awk and patch), the tar, gzip, and bzip2 archive tools, `ssh-keygen`, and QEMU, and learns the whole interface from one Readme paragraph.

The design was settled in six recorded rounds between the primary agent and an independent Codex reviewer. This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`, except where a phase below explicitly amends `CLAUDE.md` in its own commit.

## The decided end state

### The public interface

* `make` builds everything reusable: the kernel, the standalone KRFFS host tools, the utilities, ISH, the guest probes and musl test programs, the vendored userland (musl, BusyBox, Dropbear, TinyCC, pdpmake), Doom, and the test, os-simple, and os-real RAM images.
* `make test` builds what the default suite needs and runs it.
* `make clean` removes in-house objects, generated support binaries, and images. It preserves the vendor builds and the local credentials.
* `make distclean` also removes the vendor trees, the staged vendor outputs, the stamps, and the cached preflight. Credentials remain.
* `tests/run NAME` runs one named suite. The runner owns the suite registry, asks Make to build each suite's artifact prerequisites, checks the suite's runtime tools, and rejects an unknown name with the list of valid ones.
* `rach2/run.sh IMAGE` boots a built image: `test`, `os-simple`, or `os-real`.

`help`, `doctor`, `verify-style`, `verify-host`, `deps`, `full`, `compile-commands`, and the separate image-choice build targets disappear from the advertised interface. The root Readme documents the six entries above and states that the first build compiles five vendor projects and takes minutes. `make test` is a shortcut for `tests/run default` and contains no second registry. The GRUB disks, metal, recovery, and sealed media, the ISOs, and the FUSE mount tool are excluded from `make`; they remain real internal targets owned by the runner and the deployment procedures. The old target names survive this milestone as one-line aliases that print a retirement notice; removing them is deferred work for the following milestone. The compilation database generator moves to `tools/dev/` and is invoked directly; its self-test joins the `style` suite.

### The suite registry

One registry file lists every suite with five fields: the suite name, the old target it replaces, its artifact prerequisites as Make targets, its runtime tools, and an explicit membership mark for `all`. Phase 3 fills the last three fields for each suite from the old target's recipe and prerequisites at conversion time; the plan fixes the names, the exclusions, and the initial membership. The suites are the current test targets with the `-test` suffix dropped, plus the two verification suites:

* `default` (the current `make test` probe boot), `addressed`, `noentropy`, `disk`, `disk-ahci`, `disk-q35`, `bootpart`, `subset`, `stress`, `stress-ahci`, `persist`, `persist-ahci`, `cred-persist`, `crash`, `authority`, `bootline`, `exitline`, `login`, `procfiles`, `account`, `shutdown`, `reset`, `diskpolicy`, `diskdiag`, `ping`, `packet`, `raw`, `netif`, `dgram`, `dhcp`, `netconsole`, `tcc`, `make`, `selfbuild`, `ssh`, `grub`, `menu`, `install`, `sealed`, `drill`, `metal`.
* `style` (the current `verify-style` checks) and `host` (the current `verify-host` sanitizer tests).

`tests/run all` runs exactly the suites whose records carry the membership mark; every suite named above is marked at conversion, and a later suite chooses its membership explicitly. Real-machine acceptance under `tests/system/accept/` and the maintainer GRUB regeneration procedure are not suites, are never part of `all`, and remain governed by the hardware rules in `CLAUDE.md`.

The environment check stops being a target. A cached preflight runs at the start of `make`, `make test`, and each `tests/run` suite; `clean` and `distclean` bypass it and must work with a broken toolchain. It checks only what the requested work needs, using the registry's runtime-tool field for suites, and fails before compilation with the missing tool's name and an installation hint. Its cache records the host OS and architecture, the resolved tool paths and versions, and the checker's own version, so a changed environment or a changed checker invalidates it, and a matching cache returns without expensive probes. `tools/setup/` keeps a directly runnable script for machine setup reports.

### The layout

* `tests/system/` holds the behavioral test machinery: `runner/` (the suite runner), `suites/` (one entry point per suite), `checkers/` (pcap, netconsole, and other capture checkers), `fixtures/` (fixture builders and data), and `accept/` (real-machine acceptance).
* `tools/` shrinks to `setup/`, `verify/` (style and repository checks), `inspect/` (ELF, kernel, KRFFS, disk-policy, and GRUB inspectors), `image/` (disk and ISO construction), and `dev/` (the compilation database).
* Data files live beside the code that reads them. Built tool binaries go under `build/`.
* The guest probe sources under `tests/` and `tests/musl/` stay where they are; moving them is out of scope.

### The languages

The support languages end as POSIX sh plus C. Python, Expect, Perl, `gtimeout`, `parted`, `grub-editenv`, host `sha256sum`/`shasum`, and host `strings` are removed. Binary formats (ELF, MBR, FAT metadata, pcap, KRFFS images, the GRUB environment block) are parsed by small C programs compiled by the build with the host compiler; a tool that is also meaningful on Rach2 keeps its core inside the dialect TinyCC accepts. Every replacement runs beside its predecessor until differential tests, a deliberate failure, and the required verification agree under both.

The QEMU and pty orchestration is the last conversion. The C runner splits into a portable event core (input buffering, step parsing, match state, probe scoring, deadline calculation) and a host adapter (`poll`, `clock_gettime`, process groups, sockets, POSIX pty). The Python implementation remains the reference the C runner is compared against, on recorded event traces, on fault-injection children, and on live side-by-side suite runs including deliberately failing mutations, until every verdict agrees.

Python removal is proved over the tracked, project-owned, current files only: extracted vendor trees and the historical records under `dev-log/` are excluded, since the Dropbear tree carries upstream `.py` files and old milestone records legitimately quote `python3` commands. The proof is three searches: no tracked project `.py` file outside those exclusions, no `python3` invocation in any current script, Makefile, or test source, and no `.py` path or `python3` command in any current document.

### Build correctness

* Each component's build description data (source lists, per-program companion-source maps, and object lists) moves to shared manifest files included by the host Makefile and read by the matching `guest.mk`, so the response-file, utility, and ISH recipes on both sides consume one description. The comparison half of `check_guest.py` retires; its descriptor-limit, command-length, and `/src` staging checks survive under `tools/verify/`.
* Published artifacts with semantic validators are built to a temporary file, validated, and renamed into place, the pattern the kernel link and `krffs/mkimage.sh` already use. The set is: the kernel and its policy variants, musl's installed libc, both BusyBox builds with their applet tables and resolved configurations, Dropbear, TinyCC with its runtime and sysroot, pdpmake, the three RAM images and their set-id nodes, the GRUB disks, and the ISOs. Each validator is a prerequisite of a validation stamp, every consumer depends on that stamp, and a changed validator revalidates without recompiling. Ordinary object files keep their direct compile rules.
* The three images become real file targets with a complete staging closure: the stager emits the exact input inventory (files, traversed directories, modes, absent optional-file parents, manifest parameters, stager and mkfs binaries, image size), and only an affected image restages. The Doom WAD is an optional staged input tracked by that closure: its appearance, change, or removal restages os-real, and its absence is not an error.
* The vendor stamps name their whole input set through a checked-in patch manifest per vendor; musl's missing directory prerequisite and Dropbear's absent patch inputs are repaired. Every declared vendor output (musl's libc, headers, and CRT objects, the extracted crypt source `krffs` compiles, both BusyBox binaries with their applet tables and resolved configurations, Dropbear, TinyCC's binary, `libtcc1.a`, and sysroot tree, pdpmake) is modeled as an output of its build rule, so a deleted file in any class rebuilds by itself. Truncation and corruption are caught by the validation stamps: a stamp's validator re-runs whenever any declared output is missing or newer than the stamp, and a failed validation removes the stamp and the affected outputs, so the next run rebuilds the producer instead of failing repeatedly.
* Per-component fingerprints of flags, recipes, and toolchain versions replace the kernel's refuse-on-change `kflags` stamp with an automatic rebuild of the affected tree.
* The component graph keeps only real edges; the current `krffs` before `rach2` before everything chain serializes unrelated builds. `tests/musl/build.sh` gains ordinary per-program rules with `.d` tracking instead of recompiling everything. Credential generation compares before replacing, so an unchanged key keeps its timestamp.
* Vendor sub-builds join the GNU Make jobserver: each wrapper is invoked as a recursive Make recipe (through `$(MAKE)` or a `+` prefix) so the jobserver descriptors stay open, the wrappers stop choosing their own `-j` and pass the caller's `MAKEFLAGS` through to their inner make, and musl's wrapper, which must keep clearing `CC`, is reworked to preserve the jobserver variables while still clearing the compiler selection.
* Every component and vendor build prints one `component: action` line when it begins, and each vendor build reports `component: result: built in Ns` when it ends, so a first build is never silent.

### Docker and GRUB

Docker leaves every build, image procedure, and test. Six GRUB boot artifacts are pinned under `vendor/grub/` with SHA-256 digests and license provenance: `bios-boot.img` (the MBR boot code), `disk-core.img` (the ordinary disk core), `disk-core-a.img` and `disk-core-b.img` (fixed marked cores for the swap tests), `eltorito.img` (the ISO boot image), and `boot-hybrid.img` (the hybrid template, kept because `mkiso.sh` promises USB-bootable ISOs). Disk and ISO assembly becomes native: mtools for FAT work, `xorriso` for ISOs, and project C tools for the MBR layout, the GRUB environment block, and core embedding. The core builder becomes a selector that verifies the chosen pinned core's digest and copies it; it never generates a core. Complete disks and ISOs are never pinned.

Regeneration of the pinned artifacts is a maintainer procedure, not a suite. Its manifest must pin the inputs themselves, not the archive that serves them: either the exact `.deb` files vendored beside the manifest or immutable snapshot URLs whose signed `Release` and `Packages` hashes are verified, plus every `.deb` SHA-256, the GRUB source package name, version, and hash, the license files, the tool versions, the complete command and module order, the embedded configuration hash, `SOURCE_DATE_EPOCH`, and each output's size and digest. The procedure must produce identical bytes twice from clean directories and pass the `grub`, `menu`, `install`, `sealed`, and `drill` suites plus an ISO boot before the pinned files change. Docker may serve as that procedure's executor. The commit that introduces `vendor/grub/` amends `CLAUDE.md` with the explicit exception for reviewed, pinned boot artifacts, adds a digest verifier for the blobs, and converts at least one consumer to read them in the same commit, so the pinned files are exercised from their first appearance.

### FUSE

Only the mount tool needs FUSE. The KRFFS core objects compile without FUSE flags, the mount tool becomes an optional target off the default graph, and FUSE with `pkg-config` moves to an optional tier needed only for host-mounting an image. This split lands in phase 3, before the Readmes advertise the new dependency contract.

### The dependency tiers

* Build and run: GNU Make, a POSIX shell with the base POSIX utilities including awk and patch, the host C compiler, i686-elf GCC and binutils, tar, gzip, bzip2, `ssh-keygen`, and QEMU with SeaBIOS boot, SLIRP user networking, the `rtl8139` device model, the built-in IDE controller, `ich9-ahci` with `ide-hd`, and the q35 machine. RTL8168 is exercised through the host register model and real hardware, not a QEMU device. Interactive sound uses the platform audio backend QEMU was built with.
* Tests add: the OpenSSH client, mtools (also used by the internal GRUB-disk assembly), and a sanitizer-capable host compiler.
* Style checks add: cppcheck, ShellCheck, clang-format.
* Maintainer only: `xorriso`, GRUB host tools, Docker as an optional executor, Git, Homebrew, GDB.
* Optional: FUSE or FUSE-T with `pkg-config`, for host mounting only.
* Removed: Python, Expect, Perl, coreutils `gtimeout`, `parted`, `grub-editenv`, host SHA-256 tools, host `strings`.

## Standing rules for the run

* Work directly. Delegation is only for an independent review, one reviewer per diff, with the limits in `CLAUDE.md`.
* Commit in scoped units in the house style, directly on the branch, no attribution, never push. A phase may contain several commits; each commit is one logical change that leaves a working, proved tree.
* Every commit passes the style and host checks and, when it touches anything they exercise, a real boot and the SSH capstone. Until phase 3 renames them, those checks are `make verify-style` and `make verify-host`; afterwards, `tests/run style` and `tests/run host`. The renaming commit updates `CLAUDE.md`, `AGENTS.md`, the affected Readmes, and every caller together.
* A new or moved check is proved by making it fail. Drive each mutation from a script that restores the source in a trap, then touch the restored file or rebuild explicitly before trusting the next result.
* A replacement tool ships only after a differential run: old and new must agree on the same good and bad fixtures, and on at least one deliberately broken input each.
* A change that alters a contract rewrites the owning Readme in the same commit. When a target, path, or command is renamed, prove the documentation change with a repository search over the current documents, source comments, scripts, Makefiles, `AGENTS.md`, and `CLAUDE.md`; the historical records under `dev-log/` are excluded and never edited to match the present.
* Message shapes follow `CLAUDE.md`: `component: action` progress, `component: result: detail` outcomes.
* Probe success tokens and test contracts do not change. Where a suite moves, its tokens move with it unchanged.

## The phases

Do the phases in order. Within a phase, land the smallest self-contained commits that keep every intermediate tree passing the required checks. Record deviations and deferred defects in `notes.md`.

### Phase 1: arrival

Read `AGENTS.md`, `CLAUDE.md`, and this plan in full. Require a clean tree, record `git log --oneline -12`, run `make doctor`, `make full`, `make verify-style`, `make verify-host`, and `make test`, and require every command to exit zero before any change. Record two timed baselines: `make distclean && make full` for the cold build, then an immediate `make full` for the no-op. Phase 5 and phase 10 compare their `make distclean && make` and no-op `make` timings against these. Create `notes.md` and `current.md` beside this plan in the first commit: both are tracked, `current.md` is committed with each checkpoint and holds only the resume state, `notes.md` accumulates deviations and deferred defects and remains part of the milestone record, and `current.md` is reduced to a completion note in the final commit.

### Phase 2: the compilation database

Fix the generator: correct the wrapped-recipe selection and the tab escaping, parse the finished JSON before publication, and add a self-test containing the wrapped Homebrew command that currently produces the defect. Prove the self-test by restoring the defective behavior temporarily and watching it fail. The same commit removes the recorded limitation from `mk/Readme.md`. The self-test joins the style checks so it runs on every commit; the target itself is retired to a directly run `tools/dev/` script in phase 6.

### Phase 3: the interface

Land the FUSE split first: KRFFS core objects compile without FUSE flags, the mount tool leaves the default graph, and a machine without FUSE headers completes the build. Then build `tests/run` with the complete registry from the end state, wire `make test` to `tests/run default`, add the reduced public targets, the preflight, and the retirement aliases for every old public name, and update `CLAUDE.md`, `AGENTS.md`, and the Readmes in the commits that change each contract.

Proofs: unknown-suite rejection prints the valid names; one suite's dispatch fails when its expected token is mutated; the preflight names a tool hidden from `PATH` before compilation, a warmed cache detects a tool hidden after it was recorded, a changed checker or tool version invalidates the cache, a matching cache adds no measurable time, and `clean` and `distclean` succeed with the cross compiler missing.

### Phase 4: shared source manifests

Introduce the shared description files component by component, one commit each, covering the source lists, the utility program map with companion sources, and the ISH object list, with both the host recipes and `guest.mk` consuming them. Keep `check_guest.py` until each comparison is redundant, then reduce it to the surviving guest-contract checks. Prove each conversion by adding a unique nonexistent source to the shared file (both build descriptions must fail on it), editing a shared header (exactly the affected objects rebuild), and running `tests/run selfbuild`.

### Phase 5: incremental correctness

Land the work items from the design review, each with its own proof:

1. Real image file targets with the staging closure inventory; an untouched tree makes nothing, a one-file manifest change restages one image, and adding or removing the optional Doom WAD restages os-real.
2. Vendor patch manifests; adding, editing, and removing a patch each rebuild the vendor that owns it and its transitive dependents (for musl, that includes the four vendors and the musl-linked programs), and nothing unrelated.
3. Self-healing vendor outputs across every output class: delete musl's libc, a musl header, a CRT object, and the extracted crypt source, each BusyBox binary, an applet table, and a resolved configuration, TinyCC's binary, `libtcc1.a`, and one sysroot header, Dropbear, and pdpmake; each deletion rebuilds its owner and the owner's transitive dependents, and nothing unrelated. Truncate one output per vendor and require the validation path to rebuild the producer on the next run.
4. Validation stamps for every published artifact in the end-state list; editing a validator revalidates its artifact without recompiling it. A failed validation of a newly built temporary candidate leaves the previous validated artifact in place; a failed revalidation of an already published artifact removes that artifact and its stamp and fails, so the next run rebuilds it.
5. Flag and toolchain fingerprints replacing `kflags`; a changed `CFLAGS` rebuilds the affected tree automatically.
6. Only real component edges; the kernel, ISH, Doom, and the standalone KRFFS tools build independently.
7. Ordinary rules for the musl test programs; an untouched tree recompiles none of them.
8. Credential comparison before replacement; a second `make` leaves `authorized_keys` timestamps alone.
9. Jobserver integration: under the new interface, `make -j4` from clean shows a shared job budget by measuring the maximum concurrent compile processes, and no wrapper passes a private `-j`.

After the phase, a no-op `make` finishes in seconds and is measured against the phase 1 baseline, and a scripted first build shows every `component: action` start line.

### Phase 6: the layout

Move the scripts into `tests/system/` and the `tools/` role directories, one role per commit, updating every caller and the owning Readme in the same commit, with the renamed-path search from the standing rules. The compilation database generator lands in `tools/dev/` and its Make alias joins the retirement list. For each moved suite, mutate its expected token and confirm the runner reaches the moved script. The style checks widen their scope to the new paths in the same commits.

### Phase 7: small dependency removals

Replace `gtimeout` with a C process-group deadline tool; prove it with a hanging child and a hanging descendant. Move the SSH password and pty exchange into the retained Python runner and remove Expect; prove wrong-password refusal and window-size propagation still fail when mutated. Remove the Perl, host SHA-256, and `strings` uses by folding them into the project checkers with differential fixtures.

### Phase 8: Docker removal

Add `vendor/grub/` with the six pinned artifacts, their manifest, the digest verifier, its `Readme.md`, the `CLAUDE.md` exception, and one converted consumer in one reviewed commit. Convert the disk builder, ISO builder, and core selector to native mtools, `xorriso`, and the new C tools for MBR layout, environment block, and embedding, retiring `parted`, `grub-editenv`, and the container path from each consumer, then remove the Docker checks from the four tests and make the self-build's GRUB boot unconditional. Prove the conversion end to end: the native artifacts pass the `grub`, `menu`, `install`, `sealed`, and `drill` suites, a native ISO boots, and the self-build suite exercises its GRUB path with Docker absent. Run the regeneration procedure twice from clean directories and require identical pinned bytes.

### Phase 9: C conversions of the parsers

Convert the binary parsers in small groups with differential fixtures: ELF (`check_elf`, `check_kernel`, `check_link`), the kernel policy scanners (`check_diskpolicy`, `check_recovery`), MBR and GRUB images, pcap and netconsole, KRFFS image inventory, and the fixture builders, updating `tests/host/bootctl_hosttest.c` with the fixture builder it executes in the same commit. Split `check_setid.py` into subcommands over a shared C manifest-and-KRFFS library before translating; its allowlists become adjacent declarative files. Replace every inline `python3 -c` fragment in the image and suite scripts (`krffs/mkdisk.sh` and the install, menu, selfbuild, procfiles, drill, and sealed suites) with the matching C inspector or sh in the same group as its parser. Each group's Python original is removed only after the differential run and the mutation proof.

### Phase 10: the runner in C and the end of Python

Add the event-trace format to the Python runners and record real suite traces. Implement the C event core and host adapter, compare on traces (exit classification, matched step, consumed position, guest status, timeout cause), then on fault-injection children, then live per suite group. Convert the text checkers (comments, prose with its baselines, devlog index, the sealed-network configuration check, and the guest-contract remnant of `check_guest.py`) to one C checker with an sh driver, proved by identical reports over the repository and the pinned upstream corpus. Remove Python when the three scoped searches from the end state come back empty. Re-run the phase 1 measurements and the complete suite set.

### Phase 11: completion

Run `make distclean`, then `make` from scratch, then `tests/run all`, and run `tests/run selfbuild` and `tests/run ssh` again as direct evidence even though `all` contains them. Confirm the dependency tiers by completing the build on a machine profile without the removed tools. Write `dev-log/30-build/Readme.md` recording the scope, the interface, the removed dependencies, the measurements, the deviations in `notes.md`, and every verification command run. Add `30-build` to `dev-log/Readme.md` in the same commit as that record. The tree is clean, nothing is pushed, and the alias retirement is recorded as the follow-up work.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's end state and the current phase, `notes.md`, and `current.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. `current.md` names the active phase, the item in progress, the evidence already checked, and the exact next action; replace its contents instead of appending. After each phase and after each recovery, report one line: `build: phase N; item; tree clean|dirty`.
