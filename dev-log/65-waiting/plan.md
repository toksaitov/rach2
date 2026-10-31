# The release's waiting

Milestone 64's release took from 00:00 to 10:06 on 31 October, and for much of it the machines did not run cases: they hashed files and rewrote a digests file on one core, copied disk images byte by byte, rebuilt on Windows the GNU layers from source, and held the whole testing host while Windows prepared. This milestone removes the part of that waiting that can be removed without weakening what a release proves. Its incidents are in [incidents.md](../64-acpi/incidents.md).

The process fix of 31 October, 1aa72ccae, already runs WSL's full run at 20 workers and 64 GiB, checks style once per release and stages the Lenovo's payload; it is not part of this plan.

## The owner's decisions

* The release's full run builds without the host's store of step outputs since 76c090de3 (23 October), so each runner copy rebuilds the vendor and GNU builds: 55 min on the GNU toolchain in run 41112961670. Whether the full run may take its builds from the store is a choice between a faster release and one that proves those builds from their recipes. The owner answered yes on 31 October: the full run takes them from the store, and task 7 makes it so.
* The Mac and Windows prepare the in-house tree from clean at every release start (`ETUDE_PREPARATION_CLEAN=1` in release.sh), 20 min or more on the Mac each time. Whether they may keep their builds between releases is the same choice; the preparation receipt records `clean` or `retained` (posix/legacy.c:1270), so the evidence says which. The owner answered yes on 31 October: task 5 drops the clean preparation for both.

## Measured costs

| Waste | Where measured | Cost |
|---|---|---|
| Preparation hashes its 674 artifacts and rewrites the digests file once per digest kept | Mac, 31 October | 8.5 min, split not yet measured |
| Preparation reads back the rehearse images, 2.2 and 3.4 GiB, to hash them | testing host, 03:05 on 31 October | 30 min or more, 31 of 32 threads idle |
| The scheduler copies the disk inputs into a per-run store, then copies each guest's disk again | Mac, 31 October | 7.3 min, about 107 GiB, before the first guest |
| Windows rebuilds the GNU layers in its tooling guest, which has no step store | Windows, 05:15 on 31 October | 56 min or more, about 9.6 of 32 threads |
| Windows holds the whole host while it prepares | Windows, phase 589 | 2366 s with no port in use |
| The GNU host checks run `etude prepare -H` five times in turn, each make without -j, and their key covers every tracked file | code, build_host_inventory.sh:92-98, host_prepare_build.c:324-371, mk/gnu-host-checks.mk:3-6 | not timed alone |
| The end-of-milestone run repeats the suites the release's full run runs again on the same host | milestone 64 | one whole run |

Each task measures its cost again before and after its change, on each machine it touches, and records both in this file.

## Tasks

### 1. Make the preparation's hashing fast

The preparation record takes its digests without rewriting the digests file per artifact, hashes the files it must read on every core, and takes the digest of an image from the step that wrote it.

* Subtasks: measure how the 8.5 min splits between reading files and `digests_keep`, which rewrites the whole digests file and stats each of its lines for every digest it keeps (posix/store.c:596-640); keep the digests of one preparation in memory and write the file once, under its lock; hash the files a cache miss leaves on a pool of threads, on POSIX and Windows; SHA instructions in etude's own path only, since utils/sha256.c is also built into the guest program `reflash` with the cross C89 toolchain (utils/programs.mk:22), and the portable code stays the reference; the rehearse fixture writes the digest of the image it finished, and its Makefile rule enters it in the digests file, so that the preparation does not read the image back; host tests that compare every digest with the portable one, and that a digest entered for a file that has since changed is not used.
* Files: `hash_artifacts` and its helpers in `tools/etude/posix/legacy.c`; `etude_digest_file` and `digests_keep` in `tools/etude/posix/store.c`; `tools/etude/include/etude/artifact.h`; `tools/etude/preparation_record.c`; `tests/cases/machine/rehearse_fixture.sh` and its rule in `tests/system/suites.mk`; their host tests; the section "The digests file" of `tools/etude/Readme.md`.
* Done: the Mac's preparation of every suite records its digests in under 1 min, the testing host reads no rehearse image back, and the record lists the same digests as before.

### 2. Stop copying the disk inputs twice for each run

The guest and machine cases take their disks from a store that the copy of the tree keeps between runs, and boot each guest on an overlay of the stored object instead of a copy of it.

* Subtasks: the objects of a copy's runs in one store, `build/tests/objects`, distinct from a run's result store `RESULTS/store` (legacy_set.c:464, 4173), which keeps only the objects that the last preparation record names, so that it holds one commit's images; a stored object is checked against its digest before a run uses it; each guest's writable disk an overlay through `etude_store_overlay` (artifact.h:143), which the machine scenarios already use (scenario.c:1179), in place of `etude_store_copy` (legacy_guest.c:214); a host test that two runs at one commit copy nothing into the store and that a changed image is stored again; the Readme's section on the store.
* Files: `open_objects` in `tools/etude/posix/legacy_set.c`, `stock` and the disk making in `tools/etude/posix/legacy_guest.c`, their host tests, and that section of `tools/etude/Readme.md`. If the store's own code in `posix/store.c` must change, that part waits on task 1, which owns the file.
* Done: on the Mac, the testing host and Windows the first guest starts within 1 min of the preparation's end, and a second run at the same commit stores nothing.

### 3. Build the GNU host checks in parallel, with a key that is still whole

The GNU host checks build their five parts at once within one job budget, and the step copies into its tree only what the checks build and run from, so that its key, which covers every file the tree carries, changes only when one of those files does.

* Subtasks: the five `etude prepare -H` of `build_host_inventory.sh` run at once, sharing the launcher's job count among them instead of each taking all of it (routine_site.c:147); the helpers that both architectures publish under `build/tests/host-suite/tools` (host_prepare_tools.c:519-556) built once, or under a lock, before the five start; the list of files the step copies and keys made one list, narrowed to what the checks compile, read and run; a host test that a change to any file the tree carries changes the key, and that a change outside it keeps the key.
* Files: `vendor/gnu/build_host_inventory.sh`, `tools/etude/host_prepare_build.c`, `tools/etude/host_prepare_tools.c`, `mk/gnu-host-checks.mk`, `vendor/gnu/Readme.md`.
* Done: a commit that changes none of the step's files reuses its store entry, a rebuild takes at most a third of its measured time, and the GNU host suite passes on both architectures.

### 4. Let Windows' GNU build keep its work and use the host

Windows' tooling guest builds the GNU layers with a step store of its own on its persistent disk, so that it rebuilds only the layers whose keys changed, and with as many processors as its memory allows.

* Subtasks: a store on the guest's disk and `RACH2_STORE` in the job (`tooling_job.sh` sets none now, and the Makefile's default is `/var/cache/rach2-store`, Makefile:160); measure whether WSL's entries could serve Windows, since a key counts the compiler's `cc -v` and `uname` (step/step.c:368-399, 482-490) and GNU entries keep absolute build paths (mk/gnu.mk:15), and record the answer here, importing WSL's entries only if it is yes and the import is cheaper than the build; the guest's processors and memory from the grant, sized as `collection_tools.c:1450` sizes a preparation, at a GiB for each job; the size of the guest's store measured against D:, which was at 19% free; the Readme sections on the tooling machine.
* Files: `vendor/gnu/tooling_job.sh`, `vendor/gnu/tooling_image.sh`, `tools/etude/tooling.c`, `tools/etude/collection_tools.c` (the sizing function only), and those sections of `tools/etude/Readme.md`.
* Done: a second Windows release at a commit that changes no GNU recipe rebuilds no GNU layer, and a layer it does build keeps its guest's processors busy. The measurement of the host-checks key waits on task 3, which changes it.

### 5. Prepare Windows outside the whole-host claim

Windows' run prepares under a claim of only the memory and ports its tooling guest takes, and takes the whole host only for its cases.

* Subtasks: an entry that runs the collection of a finished `etude prepare routine`, with the load observer and the cleanup handed from the preparation to the collection, since the sequence now starts the observer before preparation and refuses a trace that exists (observe_sequence.c:289-295, 577-620, 741; routine_prepare.c:296); a claim in WSL's admission for the tooling guest's memory and a block of ports that WSL's launcher does not give out, so that the preparation does not meet WSL's forwarded ports, as milestone 64's incidents feared; release.sh's Windows step prepares, then holds, then runs, and release.sh drops `ETUDE_PREPARATION_CLEAN=1` for the Mac and Windows, as the owner allowed; tests of the new entry, of a failure while it waits for the whole-host claim, and of an interrupted run's cleanup.
* Files: `tools/etude/routine_sequence.c`, `tools/etude/routine_prepare.c`, `tools/etude/observe_sequence.c`, `tools/flow/release.sh`, `tools/Readme.md`, their host tests, and the Readme sections on the routine sequence.
* Done: in this milestone's release Windows holds the whole host only while its cases run, and no WSL case fails for a port or memory taken by Windows' preparation.
* release.sh builds and runs the etude of the commit it releases (release.sh:624-631, 777-784), so master's release.sh can call the new entry only once master's etude has it. This release.sh change therefore reaches master with this milestone's release, not as a separate process fix, and this milestone's own release runs master's present release.sh.

### 6. One run of the suites at the end of a milestone

The end of a milestone builds after `make clean` and runs `tests/run style` and the host tests on the testing host while Codex reviews, and leaves the suites to the release's full run, whose failures `release.sh -r` reruns alone. Milestone 64's five suite failures before its release, doom and uefi on both architectures for a missing WAD and procfiles on x86-64, would each have cost a `-r` rerun of those suites instead of a second whole run. The Mac's guest pool is tried at 14 GiB, beside its 24 GiB.

* Subtasks: the Work and Testing sections of `AGENTS.md`, and the `milestone` and `testing` skills, say so; `tools/flow/machines` gives the Mac 14 GiB for this milestone's release, and its measured use of the Mac's cores decides whether it stays.
* Subtask: `tools/setup/judges` records WSL's QEMU as `1:10.2.1+ds-1ubuntu3.3`, the package the owner upgraded to on 31 October; only `doctor.sh` prints that line.
* Files: `AGENTS.md`, `.agents/skills/milestone/SKILL.md`, `.agents/skills/testing/SKILL.md`, `tools/flow/machines`, `tools/setup/judges`.
* Done: this milestone's own end runs no suite before its release, and the Mac's share of its cores at 14 GiB is recorded here.
* As a change to the process, this goes to master by fast-forward when it is written.

### 7. The release's tests take their builds from the store

The owner allowed it on 31 October. `tools/ci/run.sh` no longer empties `RACH2_STORE` (run.sh:14), so that a runner copy takes the host's store as every other build does.

* Subtasks: the line removed; `tools/ci/Readme.md` says what a release then proves and what it takes as built; the first run after the change replaces each runner copy's in-tree GNU layers with the store's links (mk/gnu.mk:55-71), measured.
* Files: `tools/ci/run.sh`, `tools/ci/Readme.md`.
* Done: a full run reuses every vendor and GNU step whose keyed inputs and compiler identity are unchanged and whose entry is in the store.
* Measured on 31 October, before the change: run 41112961670 spent 55 min on the GNU toolchain, and run 41124008517 built i686's 56 ports again in its runner copy, 844 s by the ports' own counts. At 716db0df5 the keys that `etude build -p` gives for the toolchain and the ports of both architectures have entries in the testing host's store, so the first full run after the change removes each runner copy's own GNU layer and links those entries, building neither. The time that run takes is read from this milestone's release full run, which is the first.

## Set aside

* Quitting Windows' QEMU once its console is quiet, which both reviews raised: silence on the console does not prove that its last bytes have arrived, which is the race the 5 s grace closes (posix/qemu.c:757-788). The grace costs 5 s per shutdown, spread over 20 workers.
* Importing WSL's GNU entries into Windows unless task 4's measurement shows that the keys and paths match and that the import is cheaper than the build.

## Order

Tasks 1, 3, 4, 5 and 6 start at once. Task 2 starts at once and waits on task 1 only for a change to `posix/store.c`. Task 4 measures the host-checks key after task 3. Task 7 starts at once. Several tasks edit `tools/etude/Readme.md`, each its own section; no other file is shared.
