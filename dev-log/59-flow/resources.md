# Resources

## Why

Nothing in the process removed what finished work left, and nobody read the space left on the machines. On 2026-10-17, after phase #73 was released, the owner found finished branches in every clone, 26 helpers' worktrees on the owner's computer, and caches in every copy on the testing host, and noted that a full disk had made the host unreachable before. The host's `/tmp`, a 63 GiB tmpfs in its memory, held some 2,400 directories that tests had left, 7 GiB of them, the largest kinds made by `machine_measure_test.sh`, `tools/verify/check.sh`, `ssh_flood_test.sh`, the `runutil` tests, `usbdisk_script_test.sh`, `vfs_unmount_test.sh` and the acceptance tests. They were left by ordinary passing runs, not only by killed ones: each script's trap restored its sources and stopped its children, and removed nothing.

The owner asked Codex (`gpt-6-astra`) how to handle resources without making the pipeline larger. It traced each leftover to the line that makes it, and recommended six changes; the owner approved them, and the standing leave to delete that they need, the same day.

## What changes

* Every test removes its scratch when it passes, from an exit trap installed before the scratch is made; a test that fails keeps the directory whose path it printed, for its logs. Large disk images go under the tree's `build/tests`, not into the host's memory. `tools/verify/check.sh` removes its logs when the check passes and keeps them when it fails.
* A worker loop between issues deletes the merged branches and helpers' worktrees of its clone (`tidy` in `tools/flow/lib.sh`). A branch counts as merged by ancestry or by `git cherry`'s patch ids, never by its commit messages, since several finished branches held commits whose messages matched master's and whose text did not.
* Every loop prunes its logs past fourteen days or 1 GiB (`prune_logs`).
* `tools/flow/status.sh` reads free space, inodes and memory on both machines and ends with a verdict; `status.sh sizes` lists the largest trees. The manager frees space when the verdict is low and stops the workers that write a disk when it is critical.
* The manager's loop removes, before each check, the scratch of tests killed before their traps ran (`tools/flow/scratch.pl`): only entries named as mktemp names them, owned by an account other than root, unchanged for a day, and used by no process.
* `make` refuses to build below `RACH2_MIN_FREE_GIB`, which the loops, ci and `testing.sh` set to 40, the floor `tests/run` already kept for its runs.
* The release removes its worktree and the build's products in it once it has closed the phase.
* The Resources section of `AGENTS.md` gives the owner's standing leave: test scratch no process uses, logs past their time, build output in an idle copy, and merged branches and clean worktrees, may be deleted without asking.

## What was decided against

* Removing the GNU builds when a phase ends. A machine build takes five minutes with them and over an hour without (incidents.md); they stay, and the manager removes one architecture's whole, with its stamps, from an idle copy only when space is low.
* A cleanup service or a separate cleanup pipeline: each loop cleans what it made, at a boundary it already has.
* Sweeping `/tmp/rach2*` by name, which would take the port and host lock files and the scratch of runs in progress.
* A shared GNU cache across accounts, which every account could corrupt.

## What tells us it works

`resources: ok` in the manager's checks, no `low` that the next check does not clear, and the host's `/tmp` holding only the scratch of runs in progress.
