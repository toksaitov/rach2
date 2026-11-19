# AGENTS.md

The rules for everyone who works on Rach2, people and coding agents alike. Codex and Muse Code read this file directly; Claude Code reads it through `CLAUDE.md`, which adds what only Claude Code needs. Procedures that only some work needs are skills in `.agents/skills/`, which Codex runs as `$name` and Claude Code as `/name`:

* `milestone`: the manager's procedure, from a milestone's plan to its release.
* `testing`: the machines that run the suites, how to reach them and run a suite, and how to write a test.
* `machine`: writing the flash drive or a Lenovo disk, working on the Lenovo, and sittings with the owner.

The rules are kept short on purpose; their history is in commit messages and `dev-log/`. During a phase of the work, an agent may keep what it should remember in its own memory files; between major phases, what they hold is folded into these files and the memory files are deleted.

## Project

* Rach2 is an operating system for 32-bit and 64-bit x86 PCs, i686 and x86-64. Its aim is to run the basic software most people use, reliably, and work is weighed by how much of that software it brings within reach. It is also taught from, so its code and prose explain themselves to the students and developers who read them.
* The kernel is freestanding C89 (`-ansi`, no libgcc), cross-built with `i686-elf-gcc` or `x86_64-elf-gcc` as `ARCH` selects. The same sources build on Rach2 with the staged TinyCC and pdpmake, so kernel code stays inside the dialect both accept.
* The userland is the ported musl libc, BusyBox, Dropbear, and TinyCC with pdpmake. `vendor/gnu/` builds a larger root beside it, with glibc and GNU programs.
* Read `Readme.md` first, and a component's `Readme.md` before changing its code. `dev-log/` records how each milestone was planned and built, and is never trimmed for size.

## How everyone works

* Work as a sensible engineer, not a script. These rules describe the normal path. When something unusual happens, understand it, fix it or work around it, tell whoever needs to know, and keep going. When a rule plainly does not fit a one-off case, do the sensible thing, say so in one line, and fix the rule if the case will come again.
* Aim for the fastest correct result: speed, never at the cost of correctness or safety. Cooperate instead of waiting on each other.
* Decide on the facts, act, and hold the decision. Do not swing from one extreme to the other with each new message, and do not take the slow path for a risk that can be handled cheaply.
* Ask the owner only for the owner's decisions, which Roles lists. Decide the rest, do it, and report what was done.
* Fix every fault the work and its reviews turn up, in the work under way, without asking. A limit that Linux does not have, in networking or anywhere else, is fixed in the milestone or release under way, with Linux's behaviour as its Done, never put off to a later one; only work the size of a milestone waits, and the owner decides that. A file system or data-safety fault is fixed fully and holds the release.
* After an incident, make the one smallest change that would have prevented it, usually a line in a rule or one changed step: never a ban on the useful thing, and no new code or tooling unless the owner asks.
* Force a rare fault instead of waiting for it. After its second appearance, build the load that makes its trigger certain and run that, with tracing on; then put the same load on the fix.
* When anyone, a worker, the manager or the chat, goes round in circles on a problem without finding its cause, the manager or the chat asks for help, at the latest before a third try, the strongest first: Fable, then Codex, then Muse Code, then agy, each for what those before it leave open or disagree on. Check every answer against the code before using it.
* Report what you have read, not what you expect: a finished result, a failure the moment it is seen, or a decision that is the owner's. Trace a cause to the end before stating it, with no guesses, interim readings or retractions. Say "no change" only after reading the run's own output. Mark a figure from another agent as theirs. A record of failures is built from the transcripts, lists its writer's own failures first, and is shown to the owner before it is committed.
* Size work by its scope, in files, tasks and what waits on what, and time it by measured agent throughput, never in a person's days.

## Roles

* The owner sets the goals, approves each milestone's plan and makes the owner's decisions: a choice of taste or scope between outcomes that are each correct, the owner's hardware, repository settings, spending, and deleting what Resources does not allow. Approval is an explicit yes to the whole; answers to parts of a plan, or a reply that asks a question, are not approval.
* The chat is the owner's own Claude Code session on the Mac. It answers the owner, carries out the owner's orders and passes them to the manager, and does no long work. While work runs, it checks the manager and the loops every quarter hour and has the manager clear an idle loop or a stall at once; it reports to the owner every half hour, and a big incident at once, in the chat and on the owner's phone.
* The manager is a Claude Code session on the Mac that the chat starts. It plans each milestone, keeps the workers busy, writes its share of the code through up to three worker loops of its own, and integrates, tests and releases. It clears whatever is in the way by the quickest sound fix, without waiting for a procedure: a stuck agent, a hung machine, service or tool, a broken build. It handles a small incident itself; a big one, which stops the milestone, puts data or hardware at risk or needs the owner's decision, it handles as far as it safely can and reports to the chat.
* The workers are the worker loops, `tools/flow/worker.sh`: one in each of the five team members' accounts on the testing host, and on the Mac the owner's Codex loop and the manager's own. They take tasks from the board by themselves.
* The reviewer is Codex on GPT-6.2 Sol at high, one job at a time with no subagents, as the `codex-exec` skill says. It reviews each milestone's plan, a stack's branch at each checkpoint and at the stack's end, each time from where its last review stopped, a lone milestone's whole diff at its end, and each fix after them. No Claude subagent reviews; while codex cannot start, Muse Code stands in. agy (Gemini 4 Neon) and Muse Code (Muse Wonder Full) also review each milestone's plan and give second opinions; they are signed in only in the owner's accounts on the Mac. On a hard question the reviewers work in rounds, each round carrying the others' answers and the newest measurements, until the cause is settled.

## Milestones

| Word | On GitHub | Branch | Card on the board | Closed when |
|---|---|---|---|---|
| Milestone | a GitHub milestone | `milestone/NN-name`, or its stack's | no; cards are grouped by it | it is released |
| Task | an issue in the milestone | `task/N` | yes | its worker merges it into the milestone branch |
| Subtask | an issue under its task | none | yes | its worker finishes it |

* The manager writes each milestone's plan in `dev-log/NN-name/plan.md`: the goal as a user sees it, the owner's decisions, a few large tasks, each with its subtasks (its tests and documents among them), the files it owns and what done means, ordered so that one rarely waits for another, and the milestone's checkpoint slice: the few cases that prove its goal, sized to run in minutes. A step on the Lenovo comes at the end of its task. A plan that stacks milestones names the stack and its branch. What the plan needs from a machine is measured first.
* Codex, agy and Muse each review the plan on its own: whether the goal can be reached this way, whether workers would wait on each other or collide in the same files, and what is missing. A reviewer that is unavailable is named as missing, and the plan goes on without it. The manager checks what they raise against the code, revises the plan, and gives it to the owner with what was raised, what was changed or set aside, and each of the owner's requests that the plan leaves out and when it would start.
* Once the owner approves, the manager makes the GitHub milestone, whose description names its branch, `Branch: milestone/NN-name` or its stack's, and, unless the milestone joins a stack's branch, cuts that branch from master; it files the tasks and subtasks, orders them, and sets the tasks Ready.
* The board is the GitHub project "Rach2" of `toksaitov/rach2`, with the Status Backlog, Ready, In progress or Done. It holds the open milestones' tasks and subtasks, the owner's decisions (label `owner`) and bugs reported from outside (label `bug`), and nothing else; `status.sh` archives every other card on each look. Everyone judges the work by the board, so it matches the work at every moment: each card is assigned to whoever works it, a closed card is Done, and a milestone shows open until its release ends, while the release's card, in the release's last milestone, is In progress. A wrong board is fixed at once, and so is the code that let it go wrong.
* Master moves when a milestone is released, and for a fix to the process itself (`AGENTS.md`, `CLAUDE.md`, `.agents`, `.claude` and `tools/flow`), which goes straight to master by fast-forward once it works, since every loop runs master's. Several milestones may be open at once; their tasks are one queue, which `next` offers in the board's order, and each task is worked on its own milestone's branch.
* The manager sets the board's order, when it files the tasks and whenever a wait or a priority changes: what other tasks wait on first, and tasks that need the Lenovo early, since the machine serves one task at a time. A card the owner moves, which the chat passes on, stays where the owner put it.
* Milestones that build on each other's code form a stack, as their approved plan names it, and share one branch, the first one's `milestone/NN-name`, which each one's description names. Their tasks rebase and push onto it as any task does, so that it stays linear and a fix lands once for all of them, and the stack releases together, at its end.

## Work

* A worker loop takes the top Ready task with `sh tools/flow/board.sh next` and `take`, which keep one writer on each task. An issue labelled `linux-host` builds the GNU layer and goes only to a loop that can build it on Linux.
* Only the owner's accounts, the loops on the Mac, reach the Lenovo, but any loop may take a task that needs it. A loop of the owner's runs the step itself when the Lenovo's lock is free. Otherwise, and always on a team member's loop, the worker does everything up to the step, prepares the step whole and writes it on the task, as the `machine` skill says, and hands the task back with `board.sh release N owner-only`, or `trial` when the step boots code that has never run on the Lenovo, which waits for the manager's go. An issue labelled `owner-only` goes to the owner's loops alone, and `next` offers it only while the Lenovo's lock is free and no other is In progress; the loop that takes it runs the step and finishes the task.
* The worker writes the whole task on its task branch, compiles what it changed and commits, linking both kernels when it changed kernel code (`make ARCH=i686 rach2` and `make ARCH=x86_64 rach2`), since a missing libgcc helper shows only at the link; the commit hook formats the code and refuses what the fast style checks find. It pushes the task branch as it goes, merging in what reaches the milestone branch meanwhile so that the task branch only grows, closes each subtask when it is done, and fixes a bug it meets on the spot, naming it in the commit message instead of filing it. A small fix that comes up on its own is added as a subtask of an open task, never filed as a task of its own.
* When the task is done, the worker rebases it onto the milestone branch, compiles, pushes with `git push origin HEAD:BRANCH`, the branch its milestone's description names (rebasing again if the branch moved), closes the task and deletes its branch; the rebased task goes only to the milestone branch. A task that needs another task's code is marked on GitHub as blocked by that task, and `next` offers it once the blocker is closed.
* Wait for a long command in the foreground, or in the background with the tool's own notice; never by polling the process table. While a long run goes, read its sign of progress (cases passing, renewals, replies) on a short clock, and stop it the moment that sign stops; the notice at its end is not the only check. Read a run's result from its own output, never from a summary of it, and act on a failure the moment it shows.
* Stop a process by its PID, or by `pkill -f` with a full path that no other command line holds. A short pattern matches every process of the user whose command line contains it: `pkill -f 'up()'` once stopped a worker session, the chat and dozens of macOS daemons.
* The manager reads every account's state at least every half hour, with `sh tools/flow/status.sh`. When a loop stops or stalls, the manager stops it, commits its unfinished work to the task branch, pushes it, and gives the task to a free loop; the closed subtasks show where to go on. A loop that finds a stopped session's uncommitted edits of another task in its clone keeps them with `git stash push -m "task N"`, never with a commit that skips the hooks. It restarts a stopped loop, folds the owner's new requests into tasks, and keeps every account busy.
* If the owner's sessions must stop, the chat, the manager and its worker sessions stop together and start again together.

## Testing

* During a milestone nothing is checked but by the git hooks: no suites, no host tests, no style or document checks and no CI. The author of a new test may run that test alone, to see that it works. A worker builds and boots the whole system only to see behaviour that reading the code cannot settle: in QEMU, and on the Lenovo through the owner's accounts. A task of a checkpoint's or the end's fixes runs no check; its proof is the slice or the end's run again, once Codex has reviewed the fix.
* A commit under review is tested only once Codex has answered and its findings are fixed. No test of any kind, short or long, of test files too, runs on it before or beside the review. Fixing the findings makes a new commit, which gets its own review first. The git hooks are not a test in this sense: they run on every commit and push, and neither is a worker's trial boot of its task's build, in QEMU or on the Lenovo, which waits for no review.
* A proof is sized to the change: the narrow check that runs the changed code, on each machine the change can affect. A full run of the suites or a long soak runs only on the owner's order; the owner's approval of a milestone's plan is that order for its checkpoint's slice and for the release's runs, below, which end every stack and every milestone that releases alone. Code is final only once its real proof has passed, and the release's preparation comes after that.
* When every task of a milestone of a stack is merged, its checkpoint runs while the work on other milestones goes on. Codex reviews the branch from the head its last review reached, other milestones' commits among them, and their findings are fixed in one batch and reviewed the same way, until the head the slice will run at is reviewed whole and nothing found in what the slice runs is open; a finding elsewhere joins its own milestone's list. The manager then runs the milestone's slice at that head in QEMU on WSL, with a trial boot on the Lenovo only if the milestone touched the Lenovo's hardware, and more only when something looks wrong. A failure is fixed on the branch, reviewed, and the part of the slice that failed runs again. A checkpoint has no preparation run, no full suite, no run on the Mac or Windows and no install, and master does not move. A milestone that releases alone has no checkpoint, since its end follows at once.
* At the end of a stack, once each of its milestones has passed its checkpoint, or of a milestone that releases alone, once its tasks are merged, the manager lists every open card, known fault and pending request of its milestones and puts them all in the first batch, and makes first the changes that force a rebuild or a new install: the build itself, the vendored projects, the GNU root, the Lenovo's image. Codex reviews the branch from where its last review stopped, or a lone milestone's whole diff; with the findings fixed and reviewed, the manager rebases the branch onto master if master moved, and a conflict the rebase resolves is a fix that gets its own review. Then, with the quick checks the `milestone` skill names passed, `tools/flow/release.sh -p M SHA` builds the head in the testing host's kept preparation copy, rebuilding only what changed, runs `tests/run style` and the host tests on both architectures there, and makes each machine's release preparation as the release does. Its failures go in one list, fixed in one batch and reviewed, and it runs again at the new head. The suites wait for the release, which runs each of them once.
* `tools/flow/release.sh M SHA` (with `-c N` for each other milestone of M's stack, which it closes with M) then runs every suite on Ubuntu in WSL, installs SHA on the Lenovo and runs its native cases and acceptance on a trial boot. Until the owner says otherwise, WSL's is the release's only full run: `tools/flow/machines` gives every release `-o`, which leaves out Windows and the Mac, and they take only a check that needs one of them. On a pass it moves master to SHA, closes M and each `-c N` milestone unless an issue in it is still open, which the task that closes that issue then does, and archives its cards. A failed release is fixed at once and run again with `-r` and the same `-c N`, which keeps the passes of the last record and runs again only what failed there, and the Lenovo's install and native cases as on every release. A release is never restarted from scratch, and a run on a machine the fault does not touch is never cancelled.
* The Lenovo's installed system takes no commit that has not passed everywhere; a worker tries its own build there only on a trial boot, as the `machine` skill says.
* Whoever stops a run on the testing host confirms that its claim has left the admission ledger, `/var/lib/rach2-etude/ledger/claims`, before going on.
* A suite judges what the kernel counts, never the clock, and sets no time limit of its own.
* A test removes what it makes on every exit path, from an exit trap installed first, unless its output names a directory it keeps as evidence. Scratch goes under `TMPDIR`, which `tests/run` sets to `build/tmp`.

## Resources

* Everything the tools, tests and agents write stays under the `build/` of the clone they work in: scratch in `build/tmp`, runs in `build/runs`, caches in `build/cache`, the state of the loops and sessions in `build/flow`, and further copies of the tree in `build/worktrees`. Nothing goes in a home directory, a drive's root, a system directory or another project's. The machine-wide setup that `tools/Readme.md` lists, the test runner's site, ledger, locks and the testing host's shared caches, stays where it is.
* `sh tools/flow/status.sh` reads every disk and says low below 20% free, critical below 10% or 40 GiB; the Mac's disk works down to 5 GiB and is critical only below that. A full disk stops everyone. A disk goes to the owner only when it would fill before the work under way ends; one that is low but holds what the work left will write is not news.
* Each machine that runs the release's suites has one build copy for the end runs and the release, kept between milestones, whose unchanged vendor and GNU builds come from the machine's cache, so that no milestone builds everything from nothing. A worker builds in its own clone, and only what its task needs.
* Whoever makes something removes it: a worker its task's worktree and branch once merged, a test its scratch, the release its own. `sh tools/flow/sweep.sh` removes the rest on every machine, and the release and `sh tools/flow/stop.sh` run it, as the manager does when space runs low: merged branches, worktrees whose work is on master, a milestone branch or a branch of their own, copies whose work is on master, images and test output of finished runs, scratch older than a day, logs older than two weeks. It keeps the build copies, the caches and the evidence of anything open. A cache whose loss costs hours of rebuild goes last, only when nothing else frees enough.
* Never delete source that is on no branch, a stash, a credential or key, the Doom WAD, a pinned tarball, the GRUB images, a release's record, or anything a process uses. Do not run `git clean -fdx` or `make distclean` as cleanup.
* Use GitHub modestly, since a block of the owner's account would cost years of work. Its run files are kept three days and `status.sh` warns when they pass 1 GiB; weigh what a new upload, run or poll stores and how often it calls before adding it.

## Hardware

* The hardware is the owner's: the Mac mini, the development flash drive and the Lenovo, which is on the Mac's network. Only the owner's sessions work on it.
* The Mac mini's internal SSD, an APPLE SSD AP0512Z with serial `0ba02f1140bcba20`, is never written.
* The owner works beside the Lenovo. A step there keeps its volume at `mixer vol 1` and measures by counters; one that plays sound aloud or takes the screen is announced on its issue ten minutes before, as the `machine` skill says.
* A step that needs the owner at the machine waits for one sitting at the very end of all the open work, never in the middle of it. No milestone waits for that sitting: it releases on its automatic checks, the Lenovo's among them, and its steps for the owner move to the milestone that ends with the sitting. The owner names the sitting's time once the machine is ready; nobody asks for it sooner.
* The owner has authorized writes to the development flash drive and to any disk of the Lenovo, without asking first and without backups; report afterwards what was written. Any other target needs the owner's authorization. The `machine` skill has the identity check that runs before every write.

## Build

* `make` builds, for `ARCH` (i686 by default), the kernel, host tools, guest programs, six vendored projects, Doom, the RAM images and os-real's ext2 root, rebuilding only what changed. `tests/run NAME` runs one suite; `sh tools/setup/doctor.sh` checks a machine's setup.
* Do not run a `build_*.sh` script by hand, or `make clean` as a routine step. Everything in-house builds under the top-level `build/`.
* A source file is listed once, in its component's manifest (`rach2/sources.mk`, `utils/programs.mk` or `ish/sources.mk`), which the host Makefile and the `guest.mk` that pdpmake reads on Rach2 both include.
* The vendored projects are pinned tarballs checked by SHA-256 and built behind stamp files; do not commit their build output. `vendor/grub/` keeps eight reviewed GRUB images, which change only through `vendor/grub/regen_grub.sh`.
* The GNU root's build takes some 35 GiB for one architecture and over an hour to remake; keep it between milestones.
* When a capability is missing, look in BusyBox before writing it. That is the owner's standing decision.

## Writing

* All written language reads as if one careful author wrote it: comments, documents, commit messages, prompts and chat replies, in the voice of Kernighan and Ritchie, Stevens and Knuth, and of the Unix manual pages. Be terse, exact and concrete: the claim first, then the part that is not obvious.
* ASCII only in prose and comments, and no em-dash written as a spaced `--`. Markdown: no wrapped prose, `*` bullets, little bold, tables only for tabular data. Code comments wrap at 80 columns.
* A reply to the owner makes sense alone, since the owner often reads it on a phone with nothing else in view. Use short sentences, ordinary words and no em-dashes. Plain means free of project shorthand, of issue numbers without a description and of commit hashes, not simplified: name the real mechanism, give the measured numbers, say what is known and what is not, and use no analogies unless asked. An argument is a few short paragraphs, each opening with its point; bullets are for lists.
* Ask the owner one question at a time, in the reply's text and as its last line, with no lettered options: name the concrete thing, what changes and what is gained or risked.
* Give times in the owner's UTC+6, read from `date` or from the event's own timestamp, never carried over from earlier or guessed. Text the owner will paste elsewhere goes alone in one fenced code block.
* Issues and comments on GitHub are written like replies, the subject first and the evidence after.

### Register rule

* Write the ordinary word. `tools/verify/prosecheck.c` rejects "load-bearing", "worth stating plainly", "worth noting", "full stop", "blast radius", "tripwire", "timebox" and "seam" in committed text. Do not call code "honest" or a "fiction", write "not X, it's Y" reversals or fragments for effect, or give intentions to hardware or code.

## Comments

* A source file begins with a comment of a line or two on what it is for.
* A function has a Doxygen-compatible `/**` block before it: a one-sentence summary, then the contract (arguments whose meaning is not obvious, return values and errors, ownership, locking and context) and the reason where it is not obvious.
* Comment a structure member whose units, invariants or special values are not obvious. Inside a body, comment only a deliberate hack or a hardware or ABI fact, in one line.
* A comment states what is true now; history belongs in the commit message.

## Messages

* Diagnostics go to stderr as `component: detail`, in lowercase; progress is `component: action`, an outcome `component: result: detail`; kernel and user code use `subsystem: event: detail`. A probe's success token, such as `VMPROBE OK`, does not change.

## Documentation

* Each component has one Readme. It opens with a summary paragraph, and its `##` sections come from this list, in this order, as it needs them: Use or Running, Results, Interfaces, Sources and license, Local changes, Design, Building, Updating, Adding tests, Limits, Tests.
* A document states what the code does now, and a fact has one home. A task that changes a contract rewrites the Readme that owns it.
* A fault found in another project's code is recorded in its milestone's `dev-log/NN-name/upstream.md`, in the format `dev-log/upstream.md` gives, and indexed there, whether or not Rach2 works around it.
* Every program a system image stages, every BusyBox applet and every system call has a page or an alias under `man/`; a task that adds or changes one changes its page.

## Git

* Commit in scoped units with no attribution of any kind. The message is one line, with no trailing period, that begins with a verb in the imperative and says what the commit does.
* Workers push their task branches, and the milestone branch by fast-forward; master moves only as the Milestones section says. Never force-push, with one exception: at a milestone's or a stack's end, once its tasks are closed and nobody writes its branch, the manager rebases that branch onto a master that moved and pushes it with `--force-with-lease`, since master takes only linear history. Never pass `--no-verify`.
* The hooks in `tools/git-hooks` run on every commit and push: `commit-msg` refuses an attribution line or a message outside this rule, `pre-commit` formats the staged C files and refuses what the fast style checks find, and `pre-push` judges what a push gives a milestone branch, rebased work above all. `.claude/settings.json` turns them on for Claude Code; `git config core.hooksPath tools/git-hooks` does in any other clone.
