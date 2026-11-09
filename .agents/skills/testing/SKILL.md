---
name: testing
description: Run suites, full sets or analyses on the testing host, or write a suite. Use before starting any run a person waits for, and when adding or changing a test.
---

# Testing

When each check runs, and in what order with the reviews, is in the Testing section of `AGENTS.md`. This skill has the machines, how to run a suite on them, and how to write one.

## The machines

* The testing host is a Windows PC with an AMD Ryzen 9 7950X3D, 32 threads and 200 GB of memory. Its WSL2 distribution, Ubuntu 26.04, has 128 GB of it; `tools/Readme.md` describes its setup. The release also runs every suite on Windows itself, on the same PC, and on the Mac, as `tools/flow/machines` says, and installs on the Lenovo.
* Each team member works in their own account there, reached as `ssh rach2`, in a clone of their own; the account's `.bashrc` gives each run 8 workers and 16 GiB of guest memory. Each run claims a block of host ports for its network suites, so several clones can run the same suite at once. The PC also runs Hyper-V guests for other work.
* The owner reaches the copy `/home/testing/rach2` from the Mac through `tools/dev/testing.sh`: `sync` replaces the copy with the Mac's tree, `run COMMAND` syncs and runs the command there, `sh` opens a shell, and `wait NAME SEEN` watches a run started as `build/runs/NAME.sh`. The copy is never edited on the host. It has no GitHub login, so a `git fetch` there waits on the terminal for ever; bring a commit to it with a bundle, or set `GIT_TERMINAL_PROMPT=0`.
* The Mac runs no suites but the release's; a worker there compiles, and boots its build in QEMU or on the Lenovo.
* `sh tools/setup/prereq.sh` checks a machine for every tool the build and the suites need and prints the commands that install what is missing.

## Reaching the testing host from the Mac

* `ssh testing` lands in cmd.exe, which starts MSYS bash; an inline command with `$?` or nested quotes fails there with "The syntax of the command is incorrect". Pipe a script instead: `ssh testing 'bash -s' < script` for Windows, and `ssh -o BatchMode=yes testing "MSYS_NO_PATHCONV=1 wsl.exe -u root -- bash -s" < script` for root in WSL, whose `testing` account has no passwordless sudo. `wsl.exe` writes UTF-16, so pipe its output through `tr -d '\000\r'`.
* From root, `runuser -l ACCOUNT -c '...'` acts as one of the five team accounts (`toksaitov-d`, `-m`, `-a`, `-s`, `-v`), with that account's gh, Claude Code and clone. Each team loop keeps its state in its clone's `build/flow/claude`, as `tools/flow/workers` lists: `touch` its `stop` to end the loop after the session that is running, and its `issue` names the task it holds.
* MSYS `ps -l` gives a native process's WINPID for `taskkill //PID N //F`.

## What the host can run

* KVM works in WSL, nested under Hyper-V: kvm_amd has `nested=1` and the accounts are in the `kvm` group. A guest inside a KVM guest is slow on every exit (Linux boots in 60 to 71 s there against 2.9 s under TCG), so a third-level guest runs under TCG.
* The accounts are in the `render` group. WSL's kernel has no `CONFIG_UDMABUF`, so Venus over lavapipe in a guest there stops at Vulkan 1.2 with no swapchain.
* `vm.compaction_proactiveness` must read 20, as `/etc/sysctl.d/60-rach2-compaction.conf` sets; after WSL restarts, read it, since a boot with Docker Desktop has left it at 0.

## Running

* Run the suites a step needs as one `tests/run A B C`, which pools them across the host's threads; one `tests/run` per suite leaves the host idle.
* Never build while a run is in flight: the suites read the kernel and images from `build/`, so a build part way through voids the run. Check with `ps` before `make`.
* Start a long run detached with `setsid nohup`, as `build/runs/NAME.sh`, writing its result lines, each beginning `NAME: `, to `build/runs/NAME.out`, and watch it as a background command, as the Work section of `AGENTS.md` says. A turn never ends while a run has no watch on it.
* Read a run's rate at its first stage. A step running at a small fraction of a similar step's earlier rate has a cause; stop the run and remove it before starting again. A host suite that runs past its bound may be the disk: time a flush on the WSL root before reading the code. Leave the root's `write_cache` at `write back`: write-through hides slow flushes and risks the file system on power loss, and changing it is the owner's decision.
* The WSL root is `D:\WSL\Ubuntu-26.04\ext4.vhdx`. When its flush takes more than a few milliseconds, or drive D: needs its space back, the cure is a file-level copy onto a new VHDX, as `tools/Readme.md` describes under the testing host; a block-level copy with `Convert-VHD` keeps the slow flushes.
* Keep the whole output of a long run in a file and read all of it; never cut it with `tail` or `head`.
* Stop a run with SIGTERM, wait until every process of it has exited, and confirm that its claim has left the ledger before starting another.
* Resume a failed run at the failing step: rebuild what changed and run again only what failed.
* Believe a number only with the binary that produced it: `make` does not rebuild the recovery kernel or the install stick.
* On Windows, keep shell paths in POSIX form and convert them explicitly for native programs, and set C runtime streams to binary mode before writing framed bytes; text mode turns LF into CRLF.

## Writing a suite

* A test that kills a controller without cleanup kills and reaps it before closing its pipes. Closing them first can let the controller see a lost connection and clean up the processes the test needs orphaned.
* A suite judges what the kernel counts, never the clock. Etude stops a case only when it has made no progress for longer than the case's history on that machine allows. A time is printed for the record, not judged.
* What would make a row slow (a spin, a wait, a walk over a list, a call per page) is counted by the kernel, and the row checks each count against a bound that is the same on every machine. Where the kernel has no counter for it yet, a multiple of a baseline measured in the same boot is the last resort, and each such row is listed with its reason.
* A script removes its scratch on every exit path. Install the exit trap before the first `mktemp`, with the variable empty until then; in the trap stop and wait for the children, restore the sources, and only then remove the scratch, guarding each removal, as `[ -z "$work" ] || rm -rf "$work"`. Never put a variable that may be empty in front of a path or a glob in an `rm`. Name scratch `${TMPDIR:-/tmp}/rach2-NAME.XXXXXX`.
* A new row or suite runs with the others in its release, and earlier when a checkpoint's slice names it, and a new host test or style check at the end before its release; its author may run it alone while writing it, as the Testing section of `AGENTS.md` allows. Give it a small form, on the smallest inputs that exercise every step, and run the full size only after the small one passes.

