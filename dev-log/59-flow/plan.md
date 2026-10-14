# Milestone 59: push after a fast check, test after the merge

## Why

An agent writes a fix in 2 to 10 minutes; testing is the cost. The trial of `58-team` made every change a pull request that waited for `make test` on a branch up to date with `master`, and fifteen small pull requests took 7.8 hours. One session then fixed eight issues in 51 minutes by checking once and merging once. Teams that run many agents found the same: Anthropic tests after the merge and bisects and reverts what breaks; its sixteen parallel Claudes pushed straight to the shared repository; Cursor found that requiring a correct commit every time serialized its agents, and that an integrator was a bottleneck; Mergify's 2026 data shows teams of under fifteen rarely need a merge queue. Five accounts are five token budgets, and each works without waiting for the others.

## The rules

1. Take the top Ready issue with no open blocker, assign it to yourself and set it In progress; an issue a failure opened comes before any other. Then read the issue's timeline: if another account was assigned before you, unassign yourself and take the next one, so that no issue has two writers. Several related issues may go in one change, each still a commit of its own whose message says `Closes #N`. Helpers may write parts of it; the session that pushes runs the fast check once, for all of it.
2. Rebase onto `master`, then run the fast check: the build, `tests/run style`, and every suite the change could affect, on both architectures where it touches architecture code, with `RACH2_WORKERS=4`. Before pushing, look for an open issue with the label `hold`; while one is open, push only a repair toward a passing core run, and name it in that issue. Push to `master` by fast-forward. If another push came first, rebase, look at what it changed, and repeat only the checks it could affect. Never force-push.
3. ci runs the core tier on both architectures, one on each runner, against the same newest commit of `master`. A run is never cancelled; while it runs, only the newest commit waits for the next. The full set runs every night, the two architectures one after the other. Every `tests/run` on the host takes a shared lock on one host-wide file before its suites start, and `bigmem`, which declares 72 GiB, takes it alone, so that it waits for the runs in progress and no run starts beside it. Every result, with its commit and its kept logs, is readable by every account.
4. A suite that fails in your fast check with the failure already recorded on `master` (the same suite, architecture and row, in ci's latest result) is not yours, but you may push past it only when your change does not touch what the failing row tests; otherwise fix the failure, or show that your change passes a narrower check of that behavior. Either way, add your evidence to that failure's issue.
5. When a run on `master` fails, ci files an issue at once, with the log and the range of commits since the last pass, and the board takes it as Ready. Then ci bisects, as a workflow of its own outside the core runs' queue, started at once on either runner: it tests the failing commit again, then builds each candidate and runs what failed on it, halving the range, and names a commit only when that commit fails and its parent passes, each twice. It adds the commit to the issue, or says the range is unresolved. The next free agent takes the issue, whoever wrote the culprit: fix forward when the fix is clear, otherwise revert the named commit and reopen its issue with the log. Nobody reverts on an unresolved range; the agent narrows it first.
6. A failure that does not repeat when bisect runs it again is filed as intermittent, with both logs, and stays open. Nothing is quarantined.
7. Pushes go on while one suite fails. When the build, style, booting or the test runner itself fails on `master`, ci opens a `hold` issue at once, and closes it when a core run passes; the manager opens one too when a change on `master` has waited more than two core cycles for a passing run.
8. The owner reads what lands and reverts what they reject. Work on the Lenovo uses only a commit that a night passed whole.

## The manager

The owner's Claude Code session manages the process, on its own schedule rather than when asked.

* Every 30 minutes it reads the worker sessions, the test runs on the host, ci, the state of `master`, the board and the host's load, and writes one line to the process log.
* It fixes what it finds: a lost ci job run again, leftover jobs and locks cleared, fewer workers or fewer accounts writing while the host is overloaded, pushes held under rule 7. A worker is stuck only when neither its session nor its test has written anything for twenty minutes, since a long suite is not a stuck one; the manager stops it, confirms that its processes are gone, and only then puts its issue back in Ready, so that no issue has two writers.
* When a phase's last issue lands, it has Codex review the phase's diff, one unit after another between its checks, while the work goes on. Each finding it keeps becomes a `bug` issue, as a failure on `master` does. Nothing waits for the review: no push, no issue, no night's run and no milestone's close. Codex does nothing else unless the owner asks.
* A major incident is `master` failing for more than two core cycles, the host or the runners down, every worker stuck or out of tokens, or the same failure coming back. For one, it notifies the owner with the cause and the fix it has chosen, waits ten minutes, and applies the fix unless the owner stops it, then reports the result.
* It may change these rules in `AGENTS.md` the same way, with notice and the ten minutes. A worker session reads `AGENTS.md` when it starts, so a change applies from each worker's next session. Repository settings, the hardware and anything that deletes data still wait for the owner's yes.
* At the end of the day it reports what landed, what got stuck, where the time went and what it changed.

## What changes

* The ruleset lets the owner and the five accounts push to `master` by fast-forward; force pushes and deletion stay refused. This is the owner's decision.
* `.github/workflows/ci.yml` runs on a push to `master` and nightly instead of on pull requests. The core runs share one concurrency group with `cancel-in-progress: false`, which keeps the running run and one waiting run; the run reads the newest `master` once it starts and gives that commit to both architectures, a matrix of two with `fail-fast: false`. The nightly run and the bisect job are outside that group. ci uploads the kept logs, files the issue for a failure, starts the bisect job, and opens and closes the `hold` issue. The board adds a new issue as Backlog and cannot sort by label, so the manager moves a new `bug` issue to Ready at its next check, and a worker takes an open issue whose title begins `ci:` whatever its Status.
* `tests/run` takes the host-wide lock of rule 3, shared for every suite but `bigmem` and alone for it.
* `AGENTS.md` states the rules above in place of the pull request workflow.
* `CLAUDE.md`, `AGENTS.md` and the `milestone` skill say that Codex reviews a phase's diff once it has landed, and anything else only when the owner asks, and that nothing waits for it.

## What tells us it works

* The fast check's time with five accounts working, against its time alone. Above one and a half times, lower `RACH2_WORKERS`, then the number of accounts writing at once.
* The age of the oldest change on `master` without a passing core run. Above two core cycles, rule 7 stops the pushes.
* The agent minutes spent on repairs, conflicts and repeated checks for each change landed. If they grow, add a check where the failures come from, before the push.
