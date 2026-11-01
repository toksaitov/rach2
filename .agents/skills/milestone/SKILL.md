---
name: milestone
description: The manager's procedure for a milestone, from its plan and the plan's review to its release. Use when planning a milestone, while one runs, and at its end.
---

# Milestone

The manager runs a milestone as the Milestones and Work sections of `AGENTS.md` say. This skill has the steps.

## The plan

* Read the owner's goal, the code it touches and what earlier milestones in `dev-log/` found. Research the owner asked for goes to the owner whole.
* Measure what the plan needs from a machine before relying on it: that a tool, an accelerator or a device exists and works there.
* Write `dev-log/NN-name/plan.md`: the goal as a user sees it, the owner's decisions, and a few large tasks. Each task has its subtasks, its tests and documents among them, the files it owns and what done means. Split so that tasks rarely wait on each other and never share files, and name what each task that must wait waits on.
* Review it twice at once, each reviewer on its own. Codex: `codex exec -s read-only` in the checkout, as the `codex-exec` skill says, asked whether the goal can be reached this way, whether workers would wait on each other or collide, and what is missing. Fable: a subagent on `claude-fable-5-1` with the same questions. Check each point against the code, revise the plan, and give it to the owner with what was raised and what was changed or set aside.
* On the owner's approval: make the GitHub milestone, `gh api repos/toksaitov/rach2/milestones -f title="NN name" -f description="Branch: milestone/NN-name"`; cut and push the branch from master; file each task as an issue in the milestone, and each subtask as an issue in the milestone linked under its task (`gh api repos/toksaitov/rach2/issues/TASK/sub_issues -F sub_issue_id=ID`, ID being the subtask's `id`, not its number); mark each task that waits on another as blocked by it (`gh api repos/toksaitov/rach2/issues/N/dependencies/blocked_by -F issue_id=ID`, ID the blocker's `id`), so that `board.sh next` offers it only once the blocker is closed; then `sh tools/flow/board.sh ready N...` for the tasks. The board's own workflows add each new issue to it and set a closed one Done.

## While it runs

* Start what is not running: the team's worker loops and the owner's Codex loop, as `tools/Readme.md` says, and up to three loops of the manager's own, each in a worktree of the main checkout with a state directory of its own: `git worktree add --detach build/worktrees/mK origin/master` and `env -u TMUX tmux new-session -d -s mK -c build/worktrees/mK "sh tools/flow/worker.sh -s CHECKOUT/build/flow/mK"`, K from 1 to 3 and CHECKOUT the main checkout's path. They take tasks as every loop does, `owner-only` ones among them; `status.sh` lists them after the rows of `tools/flow/workers`.
* Keep `sh tools/flow/board.sh watch build/flow/watch` running at all times, in the background with the tool's own notice of each line it prints: in Claude Code, a Monitor. It prints a line as soon as an issue on the board gets a comment, is closed or reopened, gains or loses the label `owner` or `owner-only`, or is handed back. When the Monitor expires or the watch stops, arm it again with the same file, so that it goes on where the last one stopped. Pass each question to the owner and each hand-back to the chat at once; act on the rest as on the half-hourly check.
* Check with `sh tools/flow/status.sh` as often as `AGENTS.md` says, and act at once: hand a task over from an account out of its limit or stalled, restart a stopped loop, free disk space, clear a stuck run or machine.
* A handover: first stop what writes the task, so that nothing changes the clone while the task moves. In the stalled account's loop state directory, `touch STATE/stop`, send TERM to the process group whose id is in `STATE/session` if that file exists, and wait until the group and the loop's tmux session are gone. Then, in that clone, commit what is uncommitted on `task/N` with the message `Save unfinished work of task #N` and push it. If that push is refused because the worker had begun its final rebase, finish the task as the worker would have: compile, push to the milestone branch and close it. Otherwise release the task from that account (`sh tools/flow/board.sh release N`, run there) and comment on it where the next worker should go on. Remove `STATE/stop` and start the loop again once its account has limit left.
* Fold new requests from the owner into an open task, or file a new task with its subtasks.
* Send the chat a message at each step a person would want to know of, and at once for a big incident.

## The end

* When every task is closed and on the milestone branch, rebase the branch onto master if master moved, and push it with `git push --force-with-lease origin milestone/NN-name`, the one force push `AGENTS.md` allows.
* On the Mac, run `sh tools/flow/release.sh -p M SHA` at the branch's head, before the release. At once, it builds SHA for both architectures in a new copy on the testing host and runs `tests/run style` and the host tests there, makes Windows' preparation in its copy and the Mac's in its own, as the release makes them, the tooling machines' GNU host checks among them, and reports each machine's pass or the lines that say why it failed. It finds what only a clean tree shows, which a used build copy hides. The suites wait for the release. Start Codex's review of the whole diff at the same time, with `tools/flow/reviewer.txt` and its placeholders filled.
* Put every failure and finding in one list in the milestone's `plan.md`, fix them in one batch with the worker sessions, push, and run `release.sh -p` again at the new head.
* Once `release.sh -p` passes, start the release detached: `env -u TMUX tmux new-session -d -s rach2-release "sh tools/flow/release.sh M SHA >> build/flow/release/out 2>&1"`. Read its record when it ends. On failures, fix them in one batch and run it again with `-r`, which runs only the failed suites; on a pass it has moved master, closed the milestone and archived its cards.
* Sweep every machine as the Resources section of `AGENTS.md` says (`sh tools/flow/sweep.sh` for the testing host and the Mac), and record in the milestone's `plan.md` what was accepted: the commit, the runs and their results.
