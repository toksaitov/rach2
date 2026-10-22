# Incidents of phase #589, 2026-10-20 to 22

The faults that the process showed while phase #589 converted the suites to etude's cases, each with its cause, what it cost and its fix. Times are UTC+6, the owner's local time.

## Planning

* Most of the phase's conversions need machine cases, QEMU runs that the host drives, which #672 adds; #657, #659, #660, #662, #664, #665, #667, #668, #670, #671, #673 and #675 wait on it in whole or in part. The plan neither put #672 first nor gave it the testing host first, so workers took conversions they could not finish, and the loops of m and v paused thirty minutes again and again on issues that could not close. Over the night of 2026-10-20 to 21 one task closed.
* At 09:38 incident #798 stopped the automatic hand-out of tasks, and tasks were given out by hand from then on.

## The testing host

* Admission to the host is first in, first out, by reserved memory and processors, and a run of the scripted suites reserves the whole host. Through the night every worker's check waited 20 to 96 minutes behind another's (#792), and an interactive check of #780 had the whole host for over ninety minutes in the afternoon.
* At 22:19 a bisection of #816, whose cause was known by then, had the host while the repair's check and #672's check waited behind it. It was cancelled at 22:24.
* On the night of the 21st the host ran few tests at once while its processors were mostly idle: six or more checks waited at a load of 5 to 7 on 24 processors. Each claim reserved processors and disk as well as memory, though a run's guests use a small part of the processors it is counted for, and every shell command, such as a proof, reserved the whole host. From 01:13 on the 22nd the host admits claims by their memory alone, and from 01:29 a shell command takes an ordinary run's share (#829); the queue emptied, and eight or nine runs went at once. The new bridge was installed with a directory that only root could read, and every launch was refused from 01:07 to 01:08.

## The critical task, #672

* #672's work was ready long before it landed. One session reached the six-hour limit at 14:58 in the middle of its check. The manager stopped the next one at 16:57, after it had waited thirty minutes for host runs that had not started. The loop paused thirty minutes after each of those sessions, because it pauses after three sessions on one issue.
* Its required check passed at 18:42. The session then ran the proof of the whole legacy set, 51 faults, which no rule asked for, for two hours, and the lifecycle proof under the emulator after it. Its final check started at 22:22, and then waited in the testing host's queue behind ci's core runs and other workers' runs. The coordinating session cleared the queue for it only at 23:05, after the owner had demanded it again, by cancelling two of ci's core runs and stopping a queued run of worker a's. #672 landed at 23:11, four and a half hours after its required check first passed.
* The manager reported "no worker stuck" at every check, since the session always had a run going; nothing it checks asks whether the task that others wait on has landed. The coordinating session, which had the task under incident #798, did not watch it either.

## The last conversion, #654

* From 12:35 on the 22nd, when #670 closed, #654, the conversion of the GNU checks, was the last open blocker of #677, and every remaining task that a worker could do waited on #677. Worker s had taken #654 at 05:44, and its push was expected near 14:45, nine hours later.
* The family has 70 cases, and one run of it takes 25 to 50 minutes. The task's trial run, its check on both architectures and its proofs ran the whole family one run after another for most of those nine hours. Every phase of a proof, the unchanged one, each fault and each restored one, boots all 70 cases, though two of the catalogue's three faults touch only the ntpd case.
* The chromium case failed three of those runs, with 203, 208 and 234 disk commands against a limit of 200, because its count included the writes of the browser's second start; it now counts the completed reads of `disk0` in `/proc/diskstats`. Each failure was found only at the end of a run, and each fix cost another run. The first session reached the six-hour limit at 11:44 in the middle of a proof.
* Five loops were idle behind it: the owner's Claude Code loop from 10:30, the owner's Codex loop from 10:42, m from 12:01, a from 12:18 and d from 13:03. The other open tasks waited on #677 or on the owner. v built #677 on its own branch from 12:24, ahead of its blocker, to push once #654 lands. More workers on #654 would not have made it faster, since its time went to test runs of one tree in series.

Proposed for the next phase, none of it built yet:

* A proof runs its fault phases side by side, each in a copy of its own, once the unchanged phase has passed. #654's last proof would have taken about an hour instead of two.
* A fault's phase runs only the cases that its fault touches.
* A worker may build a task whose only open blocker is in progress, as v did with #677, and push it after the blocker lands. One level only, since work further down the chain is likely to be redone.
* The manager finds the open task that blocks the most work and the loops idle behind it, and then asks that task's worker to split what is left into issues that others take from the queue. Each issue keeps one writer.
* When a worker struggles, by three sessions on one issue or the same check failing twice, a second agent reads the issue and its evidence without changing anything and posts a diagnosis to the issue. The worker decides what to do with it.
* Several agents do not write one issue at once: they would edit the same files and wait for the same testing host.

## Core on master

* From b32524c4, pushed at 19:26, the guest cases `disk`, `disk-ahci`, `pools` and then `authority` failed now and then in ci's core runs as incomplete: the test machine booted to the debug shell and etude never started the test. The kernel's `serial_out` dropped a byte after 100000 polls of a busy transmitter, and QEMU's UART stays busy while the socket to etude is full, which with one-byte writes is after a few hundred unread bytes. On a loaded runner etude read the console late, the debug shell's greeting lost letters (`Type 'help' for avble commands.`), and etude, which waits for that line, never saw it. The scripted suites wrote the console to a file, which never fills, so the failure came with the guest cases. faff8ae43, pushed at 22:29, waits seconds for the port before it drops a byte.
* The failure reports, #808 to #811, #815 and #816, waited from 20:58 with no one assigned, since hand-out was stopped and neither the coordinating session nor the manager took them. Worker s took #815 at 22:20, after the cause was found.

## The hold

* At 21:41 ci opened hold #812, which stops every push but repairs, for failures of guest cases alone, which the rules do not hold pushes for. `run_set` in `tools/ci/run.sh` reads the failed suites from the summary after the last suite's result line; `tests/run` now prints two summaries, and on i686 the one that named the suites came before later result lines, while the last one, `run: result: incomplete`, named none, so the run was recorded as a failure of the whole set, `core`. Five workers with finished work, a, m, v, Codex and the owner's Claude worker on #672, waited on it for about an hour. It was closed by hand at 22:33 as opened in error, and #817 fixes the reading.

## Cancelled runs

* Each architecture's core run asks the admission ledger for 12 processors, so ci's two runs take all 24 of the testing host's and no worker's check is admitted for the 45 minutes they last. To let #672 through, the coordinating session cancelled two core runs and stopped a queued run of worker a's between 23:05 and 23:10, and did not look at the ledger afterwards. Their claims stayed until the ledger could confirm that their processes were gone, and in those minutes it refused every launch that could not fit beside them as `No space left on device`. ci's next core run, of #672's commit, failed at `build` on both architectures (#821, #822) without compiling anything, opened hold #823, and two checks of the coordinating session's failed the same way. The hold was closed by hand at 23:15 and core started again. #824 limits each core run to 6 processors, and a session that stops a run now confirms that its claim has left the ledger before it goes on.

The cancelled x86-64 job of [run 39640741431](https://github.com/toksaitov/rach2/actions/runs/39640741431) listed keeper 1969691 under "Terminate orphan process" at 23:12:02; the i686 job listed keeper 1943840 at 23:12:05. These were the two keepers of #820's unreleased claims. GitHub Actions' [job cleanup](https://github.com/actions/runner/blob/v2.338.0/src/Runner.Worker/JobExtension.cs) reads `RUNNER_TRACKING_ID` from each new process's environment and kills those of the job, including a keeper in its own session. Issue #820 clears the inherited marker bytes in the keeper after it forks the case, and in the compatibility lock holder. The case retains its marker. The fixture job and its mutation proof require the supervisors to survive the sweep, confirm cleanup and release admission.

## The orphaned process group

The host test of `etude-guest` closed its pipes before killing it. The helper could read the closed input, cancel the attempt and kill its process group before SIGKILL reached the helper. Issue #799 replaced the test's wait for a process ID to disappear with a FIFO that witnesses its exit, but the pipe-close race remained and failed both architectures in #833 and #834. A loaded repetition failed on attempt 23: the first process had exited, but the group member was already gone. Giving the helper 100 ms after the pipes closed reproduced both i686 failures in all three trials. Killing and reaping the helper before closing its pipes passed 200 loaded repetitions, with no completion bound changed. The testing skill now states this order for a test that needs orphaned processes. The full host suite under the added CPU load also exceeded the FAT32 and exFAT tests' bounds, recorded separately in #835.

## The manager

* The manager's check asks whether each worker wrote something in the last twenty minutes and whether new issues arrived. Each check is a new session that sees only the last ten lines of its log, its instructions are a list of things to read, and it may not take work or start helpers. So it saw neither the critical task stall, nor failures that nobody took, nor a hold that the rules did not call for. #814 records it; the owner has asked for a manager that owns the phase's progress, not for more rules.

## The coordinating session

* The owner's time zone was lost when the long session's context was compacted, and replies gave times in UTC after the owner had asked for local time. The rule is now in the Writing section of `AGENTS.md`, which every session reads at its start.
* The owner asked many times during the day that the tasks others wait on come first. The coordinating session agreed each time and still let #672 wait: for runs its check did not need, for a push stop it did not examine for an hour, and in the testing host's queue, which it treated as the normal order of things instead of clearing it.
* A time given to the owner for #672 to land, 23:30, was missed, because the push stop applied to every push and its cause was not examined until an hour later.
* Helpers waited on proofs of commits whose result was already known, and were told to stop them.

## Fixes

* faff8ae43: the serial console waits for a slow reader.
* `tools/flow/worker.sh`: a worker loop does not pause on an issue that another open issue waits on.
* `AGENTS.md`, Writing: times to the owner in local time.
* #817: ci reads the failed suites whatever the order of the summaries.
* #814: the manager, to be redesigned around the phase's progress.
* #824: ci's core runs take at most half the testing host.
* #829: the testing host admits claims by memory alone, and a shell command takes an ordinary run's share; an installed bridge is made readable by every account and tried as the testing account and as ci's before the site is switched to it.
* Admission to the testing host still has no priority for the critical task or for a repair of core.
