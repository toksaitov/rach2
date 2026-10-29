---
name: milestone
description: The manager's procedure for a milestone, from its plan and the plan's review to its release. Use when planning a milestone, while one runs, and at its end.
---

# Milestone

The manager runs a milestone as the Milestones and Work sections of `AGENTS.md` say. This skill has the steps.

## The plan

* Read the owner's goal, the code it touches and what earlier milestones in `dev-log/` found. Research the owner asked for goes to the owner whole.
* Measure what the plan needs from a machine before relying on it: that a tool, an accelerator or a device exists and works there.
* Write `dev-log/NN-name/plan.md`: the goal as a user sees it, the owner's decisions, and a few large tasks. Each task has its subtasks, its tests and documents among them, the files it owns and what done means. Split so that tasks rarely wait on each other and never share files; order the ones that must wait.
* Review it twice at once, each reviewer on its own. Codex: `codex exec -s read-only` in the checkout, as the `codex-exec` skill says, asked whether the goal can be reached this way, whether workers would wait on each other or collide, and what is missing. Fable: a subagent on `claude-fable-5-1` with the same questions. Check each point against the code, revise the plan, and give it to the owner with what was raised and what was changed or set aside.
* On the owner's approval: make the GitHub milestone, `gh api repos/toksaitov/rach2/milestones -f title="NN name" -f description="Branch: milestone/NN-name"`; cut and push the branch from master; file each task as an issue in the milestone, and each subtask as an issue in the milestone linked under its task (`gh api repos/toksaitov/rach2/issues/TASK/sub_issues -F sub_issue_id=ID`, ID being the subtask's `id`, not its number); then `sh tools/flow/board.sh ready N...` for the tasks.

## While it runs

* Start what is not running: the team's worker loops (`tools/Readme.md` says how), the owner's Codex loop, and the manager's own worker sessions on the tasks it keeps.
* Check every half hour with `sh tools/flow/status.sh` and act at once: hand a task over from an account out of its limit or stalled, restart a stopped loop, free disk space, clear a stuck run or machine.
* A handover: in the stalled account's clone, commit what is uncommitted on `task/N` with the message `Save unfinished work of task #N`, push it, release the task from that account (`sh tools/flow/board.sh release N`, run there), and comment on the task where the next worker should go on.
* Fold new requests from the owner into an open task, or file a new task with its subtasks.
* Send the chat a message at each step a person would want to know of, and at once for a big incident.

## The end

* When every task is closed and on the milestone branch, rebase the branch onto master if master moved, and push it with `git push --force-with-lease origin milestone/NN-name`, the one force push `AGENTS.md` allows.
* In the owner's build copy on the testing host, build after `make clean`, which keeps the vendor and GNU builds, and run `tests/run style`, the host tests and every suite on both architectures, as the `testing` skill says. Start Codex's review of the whole diff at the same time, with `tools/flow/reviewer.txt` and its placeholders filled.
* Put every failure and finding in one list in the milestone's `plan.md`, fix them in one batch with the worker sessions, push, and run again only the failed cases.
* Start the release detached: `env -u TMUX tmux new-session -d -s rach2-release "sh tools/flow/release.sh M SHA >> build/flow/release/out 2>&1"`. Read its record when it ends. On failures, fix them and run it again with `-r`; on a pass it has moved master, closed the milestone and archived its cards.
* Sweep every machine as the Resources section of `AGENTS.md` says (`sh tools/flow/sweep.sh` for the testing host and the Mac), and record in the milestone's `plan.md` what was accepted: the commit, the runs and their results.
