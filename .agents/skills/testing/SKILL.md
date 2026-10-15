---
name: testing
description: Run suites, full sets, analyses or mutation tests on the testing host, or write a suite or a mutation test. Use before starting any run a person waits for, and when adding or changing a test.
---

# Testing

## The testing host

* Every build, suite, full set (`tests/run all`) and analysis runs on the testing host: a Windows machine with an AMD Ryzen 9 7950X3D, 32 threads and 200 GB of memory, whose Windows Subsystem for Linux (WSL2) distribution, Ubuntu 26.04, has 128 GB of it. `tools/Readme.md` describes its setup.
* Each person works in their own account there, named after their GitHub login and reached as `ssh rach2`, in a clone of their own. The account's `.bashrc` gives each run the person's share of the machine, a command sent over ssh included: 8 workers and 16 GiB of guest memory. The `ci` runners, `rach2-ci-1` and `rach2-ci-2`, have 12 workers and 24 GiB each. Each run claims a block of host ports for its network suites, so the clones and the runners can run the same suite at once; `tests/Readme.md` says how. The machine also runs Hyper-V guests for other work, so the full set runs only in ci's night run, `night.yml`, or when the owner starts one.
* A person's own computer, whatever its system, is only a terminal to the testing host. The computer the owner edits on builds only what editing needs: a compile or one host test. A run a person waits for, such as a suite, the host tests (`tests/run host`), a full set or `make analyze`, is never started on any of them. The rules below apply on the testing host unchanged.
* The owner reaches a copy of the tree at `/home/testing/rach2` from that computer through `tools/dev/testing.sh`, and each of the owner's worker loops a copy of its own, which `RACH2_TESTING_COPY` names. `testing.sh sync` replaces the copy with the computer's tree, `testing.sh run COMMAND` syncs and then runs the command there, as in `testing.sh run tests/run all`, and `testing.sh sh` opens a shell there.
* That copy is never edited on the host, and it is synced before every run, which `testing.sh` refuses while anything still works in the copy or another of its callers holds it: `testing.sh run` does that itself, and a run started from `testing.sh sh` needs a `testing.sh sync` first. What `.gitignore` names is not sent, except the Doom WAD, so the host keeps its own `build/` and vendor trees. `tools/Readme.md` and the script's header say how the script reaches WSL and which rsync it needs.

## Running

* Try a new row (one check in a suite), suite or tool small first, on the smallest inputs that exercise every step: a 64 MiB disk where the real one is 232 GiB. Start it at full size only after it passes there.
* Never build while a run is in flight. The suites read the kernel and the images from `build/`, so a build part way through changes what the remaining suites boot and what the finished ones were judged on, and the run is void. Check with `ps` for a run in progress before `make`, and wait for it or stop it deliberately.
* Watch a run in flight. Stream its log and act on the first failing line, since most failures come in the first seconds. Do not sleep until the run ends to find out how it ended.
* An agent starts a long run detached with `setsid nohup`, as `~/NAME.sh` in the account's home, writing its result lines, each beginning `NAME: `, to `~/NAME.out` and its work under `~/NAME/`, and watches it as a background command. From the owner's computer, `tools/dev/testing.sh wait NAME SEEN` watches such a run in the `testing` account. The wait returns on a new result line, the run's end, a stall of 15 minutes or 100 minutes gone; read what it printed, act on it, and wait again with the new count. A turn never ends while a run on the host has no wait watching it. A watch that woke only at a run's end, or one killed by the two-hour limit on a background command, left finished results unread for hours, and the owner had to ask.
* Read a run's rate at its first stage. A copy, a hash or a boot running at a small fraction of a similar step's earlier rate is a symptom: the emulator's USB storage moves tens of kilobytes a second where its IDE moves megabytes, a wrong block size makes a call per sector, a wait runs until its timeout expires. Stop the run, find and remove the cause, and only then start it again.
* Keep the whole output of a long run in a file on the host that runs it, and read all of it. Never pipe it through `tail` or `head`, which throws the failure away.
* Stop a run with SIGTERM, so its cleanup releases what it has taken. Before starting it again, wait until every process of the old run has exited, since the old run's cleanup would otherwise remove files of the new one, and look for anything it left, such as a lock file or a mounted image.
* Run the suites a step needs as one `tests/run A B C`, which pools them across the host's threads. A script that calls `tests/run NAME` once for each runs them one at a time and leaves the host idle: on 2026-10-04 that put some fifty suites on course for four hours.
* Resume a failed run at the failing step: rebuild what changed, run the suite that failed, and run the others again only when the change could have touched them. Start work that does not depend on a running job beside it.
* Believe a number only with the binary that produced it. `make` does not rebuild the recovery kernel or the install stick, so a measurement on a stick, an ISO or a recovery boot first checks that the artifact has the change: its timestamp against the source's, or a string the change added.

## Writing a suite

* A suite judges what the kernel counts, never the clock. Seconds in a suite are completion bounds, which `RACH2_TIMEOUT_FACTOR` scales for a slow host, and a time is printed for the record, not judged.
* What would make a row slow (a spin, a wait, a walk over a list, a call per page) is counted by the kernel, and the row checks each count against a bound that is the same on every machine. Where the kernel has no counter for it yet, a multiple of a baseline measured in the same boot is the last resort, and each such row is listed with its reason.

## Mutation tests

* A new test, a suite or a row, is trusted only after it has been made to fail, in three steps: the unmodified test passes; a mutation, a deliberate fault compiled into the artifact the test runs, makes it fail; with the source restored and the artifact rebuilt, the test passes again.
* Drive the mutation from a script that restores the source in a trap, since a mutation can hang the suite and outlive the command that applied it. Stop the test processes before restoring a source they share.
* Copying the source back does not make `make` rebuild it, so touch the file or rebuild explicitly before believing the result.
* One mutation shows that one fault is caught, not that the whole test is sound.
* A script that changes sources in the tree holds it: it sources `tests/system/runlock.sh` and calls `runlock_hold` before it reads its arguments, so that no run builds or boots the tree while a fault is in it.
* Every fault script takes `--texts`, which finds each fault's text in the tree's sources by the script's own rule and builds nothing; `tests/run style` runs them all through `tools/verify/faulttexts.sh`. Give a new fault script the mode when it is written, so that a source change that moves a faulted line fails style rather than the next proof.
