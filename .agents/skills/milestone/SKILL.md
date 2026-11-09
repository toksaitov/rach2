---
name: milestone
description: The manager's procedure for a milestone, from its plan and the plan's review to its release. Use when planning a milestone, while one runs, and at its end.
---

# Milestone

The manager runs a milestone as the Milestones, Work and Testing sections of `AGENTS.md` say. This skill has the commands and the order of the steps.

## The plan

* Read the owner's goal, the code it touches and what earlier milestones in `dev-log/` found, and the research already in the milestone's own directory, such as `dev-log/76-graphics/research/`. Research the owner asked for goes to the owner whole.
* Measure what the plan needs from a machine before relying on it: that a tool, an accelerator or a device exists and works there.
* Write `dev-log/NN-name/plan.md` as the Milestones section of `AGENTS.md` says. Split so that tasks never share files, and name what each task that must wait waits on. Put a step on the Lenovo at the end of its task, and split a task that would go on with much code after one. Name the checkpoint slice: the cases, suites and runners, in QEMU on WSL and on the Lenovo when the milestone touches its hardware, that prove the goal in minutes. Milestones that build on each other's code form a stack on one branch.
* Start the four reviews at once, each on its own with the questions `AGENTS.md` gives: Codex with `codex exec -s read-only` in the checkout, as the `codex-exec` skill says; Fable as a subagent on `claude-fable-5-1`; and agy and Muse as the `second-opinion` skill says.
* On the owner's approval:
  * make the GitHub milestone: `gh api repos/toksaitov/rach2/milestones -f title="NN name" -f description="Branch: milestone/NN-name"`, naming the stack's branch for a milestone of a stack;
  * cut the branch from master and push it, unless the milestone joins a stack's branch;
  * file each task as an issue in the milestone, and each subtask as an issue in the milestone linked under its task with `gh api repos/toksaitov/rach2/issues/TASK/sub_issues -F sub_issue_id=ID`, ID being the subtask's `id`, not its number;
  * file a task that only ends on the Lenovo with no `owner-only` label, since its worker adds the label at that step; only a task that is nothing but a step on the Lenovo is filed with it;
  * mark each task that waits on another as blocked by it with `gh api repos/toksaitov/rach2/issues/N/dependencies/blocked_by -F issue_id=ID`, ID the blocker's `id`, so that `board.sh next` offers it only once the blocker is closed;
  * run `sh tools/flow/board.sh ready N...` for the tasks, and read its exit status: it adds an issue the board lacks and fails loudly when it cannot set one Ready. The board's own workflows add each new issue to it and set a closed one Done;
  * order the queue with `sh tools/flow/board.sh move N [AFTER]`, which puts N right after AFTER, or first: the tasks that others wait on first, and the tasks that need the Lenovo early.

## While it runs

* Start what is not running: the team's worker loops and the owner's Codex loop, as `tools/Readme.md` says, and up to three loops of the manager's own, each in a worktree of the main checkout with a state directory of its own: `git worktree add --detach build/worktrees/mK origin/master` and `env -u TMUX tmux new-session -d -s mK -c build/worktrees/mK "sh tools/flow/worker.sh -s CHECKOUT/build/flow/mK"`, K from 1 to 3 and CHECKOUT the main checkout's path. They take tasks as every loop does, `owner-only` ones among them but not `linux-host` ones, which need the GNU layer's Linux host. `status.sh` lists them after the rows of `tools/flow/workers`, and says who holds the Lenovo's lock and since when, so that a lock left by a stopped loop is seen and removed.
* Keep `sh tools/flow/board.sh watch build/flow/watch` running at all times, in the background with the tool's own notice of each line it prints: in Claude Code, a Monitor. It prints a line as soon as an issue on the board gets a comment, is closed or reopened, gains or loses the label `owner` or `owner-only`, or is handed back. When the Monitor expires or the watch stops, arm it again with the same file, so that it goes on where the last one stopped. Pass each question to the owner and each hand-back to the chat at once; act on the rest as on the half-hourly check.
* Check with `sh tools/flow/status.sh` as often as `AGENTS.md` says, and read the board in the same look. Act at once: hand a task over from an account out of its limit or stalled, restart a stopped loop, free disk space, clear a stuck run or machine, put a wrong card right.
* A handover: first stop what writes the task, so that nothing changes the clone while the task moves. In the stalled account's loop state directory, `touch STATE/stop`, send TERM to the process group whose id is in `STATE/session` if that file exists, and wait until the group and the loop's tmux session are gone. Then, in that clone, commit what is uncommitted on `task/N` with the message `Save unfinished work of task #N` and push it. If that push is refused because the worker had begun its final rebase, finish the task as the worker would have: compile, push to the milestone branch and close it. Otherwise release the task from that account (`sh tools/flow/board.sh release N`, run there) and comment on it where the next worker should go on. Remove `STATE/stop` and start the loop again once its account has limit left.
* Keep the queue in order with `board.sh move` as tasks close and waits change; a card the owner moved stays where the owner put it.
* A task handed back `owner-only` at its step on the Lenovo goes to a loop of the owner's as soon as the Lenovo's lock is free. Read its comment, and for code that has never run on the Lenovo say go on the issue before the boot, as the `machine` skill says. Prepared steps that need the same kernel may share one boot.
* Fold the owner's new requests and small fixes into an open task as subtasks; file a new task only for work of a task's size.
* When a long run starts, name its sign of progress and arm a check that fires on silence, in Claude Code a Monitor, besides the notice at its end.
* Send the chat a message at each step a person would want to know of, and at once for a big incident. While the owner discusses something with the chat, send only the end of the work or a big incident.

## The checkpoint

* When every task of a milestone is closed and on the branch, run its checkpoint while the other milestones' work goes on. Have Codex review what reached the branch since its last review, with `tools/flow/reviewer.txt` and its placeholders filled, and run nothing on the branch until it answers. Put its findings in one list in the milestone's `plan.md`, fix them in one batch with the worker sessions, and have Codex review the fix commits in turn.
* Then run the slice the plan names, in QEMU on WSL, and its trial boot on the Lenovo, under the lock, if the milestone touched the Lenovo's hardware. Read each result from the run's own output. Run more only when something looks wrong, and say why.
* A failure is fixed on the branch, reviewed, and the part of the slice that failed runs again.
* Record in the milestone's `plan.md` the commit, what ran and its result, and tell the chat. A checkpoint moves no master and closes no milestone.

## The end

* The end comes for a milestone that releases alone when its tasks are closed, and for a stack when each of its milestones has passed its checkpoint.
* List in one place, the last milestone's `plan.md`, every open card, known fault and pending request of the milestones, and fix them all in the first batch, so that nothing is found late. Make first the changes that force a rebuild or a new install: the build itself, the vendored projects, the GNU root and the Lenovo's image, so that a later fix costs only a rerun.
* With every task closed and on the branch, rebase the branch onto master if master moved, and push it with `git push --force-with-lease origin milestone/NN-name`, the one force push `AGENTS.md` allows. Milestones that ran apart and that the owner wants released together are joined first: rebase each later branch onto the head of the one before it and push it, so that the last one's head has them all. The end runs and the release take the head once, as M, the last milestone of the stack or the join, with each other milestone given by `-c N`.
* Have Codex review what changed since the last checkpoint's review, or a lone milestone's whole diff, with `tools/flow/reviewer.txt` and its placeholders filled, and run nothing on the branch until it answers. Put its findings in the same list, fix them in one batch with the worker sessions, and have Codex review the fix commits in turn.
* Then, on the Mac, run `sh tools/verify/hookcheck.sh range origin/master SHA`, which takes a minute or two and finds most of what the style run would, and the narrow proof of each fault the milestone set out to fix, on each machine it affects. A failure is fixed, reviewed, pushed to the milestone branch and proved again.
* Once those pass and none is still running, run `sh tools/flow/release.sh -p M SHA` at the branch's head. At once, it builds SHA for both architectures in a new copy on the testing host, `build/worktrees/prepare` of the owner's, which it removes when it ends, and runs `tests/run style` and the host tests there, makes Windows' preparation in its copy and the Mac's in its own, as the release makes them, the tooling machines' GNU host checks among them, and reports each machine's pass or the lines that say why it failed. It finds what only a clean tree shows, which a used build copy hides.
* Add every failure to that list, fix them in one batch, have Codex review the fix, push, and run `release.sh -p` again at the new head.
* Before the release, clear each trap that has failed a release before for a cause outside the code: the Lenovo's part starts from a tmux server with macOS's Local Network access, as `tools/Readme.md` says. Take the Lenovo's lock for the release, as the `machine` skill says.
* Once `release.sh -p` passes, start the release detached: `env -u TMUX tmux new-session -d -s rach2-release "sh tools/flow/release.sh [-c N]... M SHA >> build/flow/release/out 2>&1"`, with SHA on M's branch, the last joined one's. Watch it as any long run. On a failure, act at once while the machines it does not touch keep running: reproduce it, fix it, have Codex review the fix, push NEWSHA to M's branch, and run `release.sh -r [-c N]... M NEWSHA`, with every `-c N` of the first release, which keeps WSL's, the Mac's and Windows' passes from the last record and runs only what failed, with the Lenovo's install and native cases as on every release. On a pass it has moved master, closed the milestone and archived its cards.
* Read the sweep's output, which the release keeps as `build/flow/release/*.sweep`, and record in the milestone's `plan.md` what was accepted: the commit, the runs and their results.
