---
name: testing
description: Run suites, full sets or analyses on the testing host, or write a suite. Use before starting any run a person waits for, and when adding or changing a test.
---

# Testing

## The machines

* The testing host is a Windows PC with an AMD Ryzen 9 7950X3D, 32 threads and 200 GB of memory. Its WSL2 distribution, Ubuntu 26.04, has 128 GB of it; `tools/Readme.md` describes its setup. The release also runs every suite on Windows itself, on the same PC, and on the Mac, as `tools/flow/machines` says, and installs on the Lenovo.
* The distribution's disk is `D:\WSL\Ubuntu-26.04\ext4.vhdx`, which grows as it fills and does not shrink when files are deleted. To give space back to drive D:, run `fstrim /` in WSL, stop Docker Desktop with `docker desktop stop` and run `wsl --shutdown`. Rename the file to `ext4.old.vhdx`, so that nothing can start from it, then write a new file with `Convert-VHD -VHDType Dynamic -BlockSizeBytes 1MB`, which leaves the source whole; `Optimize-VHD` in place has broken this disk once. D: needs room for the new file, which is at most the used space plus the free extents too small to drop. Check the new file with `Test-VHD`, `qemu-img check -f vhdx` and `qemu-img compare -f vhdx -F vhdx` against the old one, using `C:\Program Files\qemu\qemu-img.exe`, as the build under `D:\rach2` has no VHDX driver, copy the old file's ACL with `Set-Acl`, and rename the new file to `ext4.vhdx`. Start WSL and Docker Desktop, and delete the old file once the checks pass.
* Each person works in their own account there, reached as `ssh rach2`, in a clone of their own; the account's `.bashrc` gives each run 8 workers and 16 GiB of guest memory. Each run claims a block of host ports for its network suites, so several clones can run the same suite at once. The PC also runs Hyper-V guests for other work.
* The owner reaches the copy `/home/testing/rach2` from the Mac through `tools/dev/testing.sh`: `sync` replaces the copy with the Mac's tree, `run COMMAND` syncs and runs the command there, `sh` opens a shell, and `wait NAME SEEN` watches a run started as `build/runs/NAME.sh`. The copy is never edited on the host. The Mac runs no suites but the release's; a worker there compiles, and boots its build in QEMU or on the Lenovo.
* `sh tools/setup/prereq.sh` checks a machine for every tool the build and the suites need and prints the commands that install what is missing.

## Running

* At the end of a milestone the manager runs style, the host tests and each machine's release preparation at once with `sh tools/flow/release.sh -p M SHA` on the Mac, before the release starts. Its style and host tests run in a new copy on the testing host, `build/worktrees/prepare` of the owner's, which it removes when it ends, so a fault that only a clean tree shows is found there and not in the release.
* Run the suites a step needs as one `tests/run A B C`, which pools them across the host's threads; one `tests/run` per suite leaves the host idle.
* Never build while a run is in flight: the suites read the kernel and images from `build/`, so a build part way through voids the run. Check with `ps` before `make`.
* Start a long run detached with `setsid nohup`, as `build/runs/NAME.sh`, writing its result lines, each beginning `NAME: `, to `build/runs/NAME.out`, and watch it as a background command. Act on the first failing line; do not sleep until the run ends. A turn never ends while a run has no watch on it.
* Read a run's rate at its first stage. A step running at a small fraction of a similar step's earlier rate has a cause; stop the run and remove it before starting again.
* Keep the whole output of a long run in a file and read all of it; never cut it with `tail` or `head`.
* Stop a run with SIGTERM, wait until every process of it has exited, and confirm that its claim has left the ledger before starting another.
* Resume a failed run at the failing step: rebuild what changed and run again only what failed.
* Believe a number only with the binary that produced it: `make` does not rebuild the recovery kernel or the install stick.
* On Windows, keep shell paths in POSIX form and convert them explicitly for native programs, and set C runtime streams to binary mode before writing framed bytes; text mode turns LF into CRLF.

## Writing a suite

* A test that kills a controller without cleanup kills and reaps it before closing its pipes. Closing them first can let the controller see a lost connection and clean up the processes the test needs orphaned.
* A suite judges what the kernel counts, never the clock, and sets no time limit of its own. Etude stops a case only when it has made no progress for longer than the case's history on that machine allows. A time is printed for the record, not judged.
* What would make a row slow (a spin, a wait, a walk over a list, a call per page) is counted by the kernel, and the row checks each count against a bound that is the same on every machine. Where the kernel has no counter for it yet, a multiple of a baseline measured in the same boot is the last resort, and each such row is listed with its reason.
* A script removes its scratch on every exit path. Install the exit trap before the first `mktemp`, with the variable empty until then; in the trap stop and wait for the children, restore the sources, and only then remove the scratch, guarding each removal, as `[ -z "$work" ] || rm -rf "$work"`. Never put a variable that may be empty in front of a path or a glob in an `rm`. Name scratch `${TMPDIR:-/tmp}/rach2-NAME.XXXXXX`.
* A new row or suite runs with the others in its milestone's release, and a new host test or style check at the milestone's end; its author may run it alone while writing it, to see that it works. Give it a small form, on the smallest inputs that exercise every step, and run the full size only after the small one passes.
