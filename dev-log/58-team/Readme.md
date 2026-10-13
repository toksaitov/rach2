# The team workflow, tried end to end

On 2026-10-13 the workflow of `AGENTS.md` ran once from the board to the Lenovo, with the five team accounts doing the work. The owner's session put small faults from the tree on the board as issues of phase #4, and each account's Claude Code, started as `claude -p` over SSH in a `tmux` session of its own, took the top Ready issue, fixed it, checked it, pushed, opened the pull request with auto-merge, and came back to it when it fell behind `master`, conflicted or failed. Then the owner's session ran the night's work: Codex's review of the phase's diff, the build from clean, the full set on both architectures, and a kernel update of the Lenovo. This record gives what ran, what broke, and what the workflow costs, so that the next change to it starts from measurements.

## What ran

* Thirteen pull requests merged, each from a branch named for its issue: the workflow's own setup (#1), two of the owner's fixes to the host's setup (#3, #10), and ten of the team's (#11 to #14, #19, #21 to #24, #30). From opening to merge they took 22 minutes at the least, 158 at the most, and 61 at the median.
* Three pull requests met a conflict with `master`, and their authors resolved it without help: d's in `.agents/skills/testing/SKILL.md` against #10, v's twice in `tests/Readme.md` against #22 and #21, and a's rebase of `tests/run` over #24, where a also checked that the two changes do not interfere.
* The team found and filed faults of their own: d's #15 (a run piped to `tail` stopped the `tail`) and v's #25 (the usb suite's hub-pull row failed once under a full ci pool), which v also judged to be outside the change and ran again.
* Codex reviewed the phase's merged diff in six units of at most six files each, and gave a seventh to the design of faster test runs. Its findings that held became seven issues, #16 to #18, #20, #26, #27 and #29; the first four were fixed in the same phase.
* The night's work, at `a4bd4535`: the build from clean and `make machine ARCH=x86_64`, both exit 0; the Lenovo's kernel update through `bootctl kernel`, one trial boot, back in 20 seconds, and `bootctl commit`; the scripted metal acceptance, which passed at `f85d74c7` once #30 had merged; and the full set on both architectures under TCG. Each set passed 92 suites. On i686 `gnu` failed, and on x86-64 `gnu` and `grow`. Run alone, `grow` passed and `gnu` failed again. The night report is #33.

## What broke, and what was fixed

* The demo keys were checked out with mode 644 by `actions/checkout`, and ssh refused them in the ssh and metal suites. `tests/run` now sets them to 600.
* `powershell.exe` fails when started from a directory inside another account's home, which is mode 750, so the free-space check failed in every account but the owner's. `tests/run` starts it from `/` (#3).
* `claude` was not on `PATH` for a command sent over SSH, since a non-login shell reads `.bashrc` alone. Each account's `.bashrc` and `/etc/skel/.bashrc` now add `~/.local/bin`.
* Two trees on one host forwarded the same host ports, and ci failed in the metal suite when two runs met. `tests/run` now claims one of ten 1000-port blocks for each run (#14), and the review's findings on the claim became #16, #17, #18, #20, #27 and #29.
* `metal_accept.sh` took an installed machine whose root is named by `GPTID=` for a stick boot, and failed two rows on the Lenovo, whose disk has a GUID table since its `--update`. The night's run found it; it was filed as #28, m fixed it with a GPT row in the installed acceptance and a mutation that drops the `GPTID=` form, and the fix merged as #30.
* The night's first full set ran under KVM, because its script was started through `testing.sh sh`, which does not set `RACH2_QEMU_ACCEL=tcg` as `testing.sh run` does. On this host's nested KVM the ext2 suite's zombie row took about 45 seconds against its 40-second step bound. Both sets ran again under TCG.
* Under TCG the gnu suite's `autotools` row failed at its 900-second bound on both architectures and when run alone. A run with the bound raised to 2400 seconds showed GNU sed's `configure` still at work after 870 guest seconds, and still to be followed by the `make` of sed. The bound was set under KVM, where `configure` takes about 50 seconds (`50-shortcomings/plan.md`, entry 294). With KVM the ext2 suite fails instead, so no single accelerator passes the full set on the testing host. This is #32, and it is open.
* `grow` failed once in the x86-64 set, under the set's load: the guest's ATA driver reported "no answer in 500 ticks: status 0xC0" during the seed write, and the row, which does not watch for `machine-grow: failed`, waited for the runner's 300-second idle limit. It passed alone. This is #31.
* The owner's dispatcher, the loop that sent an author back to a pull request that fell behind, conflicted or failed, missed conflicts at first, and later started two sessions for one head when GitHub reported it first as behind and then as conflicting. It now keys each dispatch on the pull request's head commit and one of two reasons, update or failure.
* A `claude -p` session does not exit while a background `Monitor` it started is still running; a leftover `tail -F` kept one alive until it was killed.
* `gh project item-list` shows a new item minutes late; a session that picked its task from that listing skipped #17. The GraphQL `projectItems` of the issue is current.

## What the workflow costs

* ci is `make test` on two runners of the testing host, 17 to 20 minutes for one run alone and 25 to 36 minutes with both runners and the team's own suites on the host.
* Strict up-to-date merging lets one pull request merge per ci round: each merge makes every other open pull request behind, its author rebases, and ci runs again. Of the runner minutes spent on pull requests by 15:20, 338 went to runs that passed, 318 to runs a later push cancelled, and 54 to failures.
* A rebase took an author's session 5 to 20 minutes, because each one ran `tests/run style` and its suites again before pushing.
* Use of the subscription was small: each account's five-hour window stood at 2 to 5 percent at the end, and the 44 sessions together came to about 33 dollars at API prices.

## Where a ci run's time goes

The run of #21 took 17 minutes 18 seconds, with the host's load average near 4 of its 32 threads:

* `style`, 162 seconds, then `host`, 409 seconds, then the core pool: `make test` runs three `tests/run` commands one after another (`Makefile:966`), and `run_set` would run style and host before the pool even in one command (`tests/run:979`).
* `host` runs its 156 tests one at a time; `exfat_hosttest` takes 160 seconds and `fat32_hosttest` 103.
* The pool, 12 suites at once under TCG, ends 35 of its 36 suites in 3.7 minutes. The runner's selftest takes 313 seconds; it is line 122 of the 128 in `tests/registry`, whose order is the schedule, so it starts last and runs alone at the end.
* The pool is not short of memory: the core suites declare 8576 MiB in all against a budget of 24576.

The machines boot under TCG because the ATA driver polls a status port after every sector it writes, and on this host each poll is an exit through two hypervisors (`tools/Readme.md`).

## What would make it faster

Nothing here is changed yet. In order of the time saved for the work, with the arithmetic from the run above:

1. Start the longest work first: `runner` at the top of `tests/registry`, `exfat_hosttest` and `fat32_hosttest` first in `host.sh`. Up to two minutes, for a few lines.
2. Run style, host and the pool at once after one shared build, and give `host.sh` a bounded queue of jobs, launched through `runutil` so that each test's signal dispositions are reset, which `pwauth_hosttest` checks. The 884 seconds of serial work become about 313, the runner's selftest, and a run about 6 to 7.5 minutes.
3. Split the runner's selftest into independent scenarios with fixtures of their own, keeping together those that test an order. Four groups bring it from 313 seconds toward 80, and the pool then ends with its next longest suite, at about 222.
4. Shard the exFAT and FAT32 interruption matrices by cutoff, with a check that the shards' case identifiers together are the original set, and keep the pristine fixture image instead of building it again for every load.
5. Stop discarding ci work. Without a change to the host, a dispatcher can rebase only the pull request whose ci ends next. Dropping strict up-to-date merging is cheaper still, since a `pull_request` run already tests the pull request merged with `master` as it was when the run started; a run on `master` after each merge and the night's full set would catch two changes that pass apart and fail together. GitHub's merge queue is not available to this repository while it is private and owned by a user: GitHub offers it to an organization's public repositories, and to its private ones on Enterprise Cloud.
6. Skip a suite whose inputs are unchanged since it passed. The runner already hashes each run's scripts and products (`tests/run:830`); a pass recorded under those hashes would let a pull request that changes only documentation skip the suites, as Bazel does with `--cache_test_results=auto`. Only passes are recorded, so a failure always runs again, and the night's full set runs without the record.
7. Disk images by reflink or qcow2 overlay instead of whole copies in the ssh, metal and crash suites, as grow already does with backing files; one overlay must last through a sequence that tests persistence.
8. KVM for the suites that do not write a disk by PIO, with TCG kept for the rest and for the processor behaviour the two differ on, such as debug watchpoints; each suite measured first. A write path in the ATA driver that completes by DMA or by interrupt would take away most of the exits, make KVM usable for every suite, and help the driver on real hardware too. A native Linux on the testing host would give first-level KVM. Until one of these is done, the gnu suite needs bounds for TCG or KVM of its own (#32).
9. Boot once and restore a saved machine state for the suites that boot many similar machines, such as usb and memory; cold boots stay for start-up, memory maps, enumeration, recovery and persistence. Restored clones need fresh entropy and a network identity of their own, as the Firecracker work on snapshot uniqueness describes.
10. A network namespace for each suite would let every machine forward the same ports and end the limit of ten blocks, but saves no time today; Ubuntu restricts unprivileged user namespaces through AppArmor, so it needs a profile.
11. Two runners on one host add places in the queue, not processors. Once a run is parallel inside, a host-wide budget of processors, compilers and disk writers across ci and the team's accounts is worth more than a third runner.
12. Two smaller faults in the runner bear on this: the selftest's `hostpeer` rows use the fixed port 47123, outside the table, so two runner selftests at once collide; and `tests/run` takes KVM whenever `/dev/kvm` opens, so a script that forgets `RACH2_QEMU_ACCEL=tcg` gets the slow path, as the night's first set did.
