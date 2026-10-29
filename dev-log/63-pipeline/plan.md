# The pipeline of milestones

Milestone 62 showed that the process of `AGENTS.md` cost more than the work it ran. The rules had grown to some seven thousand words; every push waited on its own suites; the manager, the reviewer and the notifications were loops of shell scripts and headless checks whose state no person read, and whose messages were often lost; a phase's members, groups, holds and `ci` issues each had their own queue rules; and the release waited on a review loop and a core run that were turned off part of the time. The owner's notes on it are in [owner-notes.md](owner-notes.md).

This milestone replaces that process with a shorter one and tries it on a small piece of hardware work, the Lenovo's backlight, power button and keys.

## The process

The words are those of GitHub. A milestone is a GitHub milestone with a branch of its own, `milestone/NN-name`, which its description names on a `Branch:` line. A task is an issue in the milestone, worked on a branch `task/N` and merged into the milestone's branch by the worker that wrote it. A subtask is a sub-issue of a task, so that the card of the task shows its progress; it has no branch.

The owner approves a milestone's plan after Codex and a Claude Fable subagent have reviewed it. The manager, an interactive Claude Code session in tmux that the coordinating chat session talks to, gives out the tasks to the five team loops, the owner's Codex loop and its own worker sessions, follows them, moves the work of an account out of limit to another through its pushed task branch, and handles what goes wrong. During a milestone the only checks are a compile and the commit hook, which formats C files. At its end the manager builds the branch after `make clean`, which keeps the vendor and GNU builds, and runs the style check, the host tests and every suite on both architectures while Codex reviews the diff; it fixes what they find in one batch, runs again only what failed, and starts `tools/flow/release.sh`, which runs every suite on WSL, Windows and the Mac, installs the result on the Lenovo, and on a pass fast-forwards `master`, closes the milestone and archives its cards.

## What changed

* `AGENTS.md` came down to some 2,500 words: the project, how every agent works, the roles, milestones and their terms, the work, and the rules for code, tests, resources, hardware, writing, documentation and git. `CLAUDE.md` says how the chat, the manager and its worker sessions run.
* The `milestone` skill is the manager's procedure, from the plan and its reviews to the release; the `testing` and `machine` skills lost what described the old loops.
* The manager loop, the reviewer loop, the progress report and the script that sent notices to the owner's phone were removed; the chat session sends the few that matter itself. `board.sh` works by milestones; `worker.sh` gives a session its task and the milestone's branch; `release.sh` takes a milestone and a commit and files no issues; ci keeps `full.yml` for the release and loses the core workflow and bisection.
* The commit hook formats the staged C files and refuses nothing; the push hook is gone. The text and prose rules run in `tests/run style` at the end of a milestone.
* Everything the loops make is cleaned by the one who made it, and the manager sweeps every machine at the end of each milestone and whenever space runs low, without forcing a build from nothing.
