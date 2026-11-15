@AGENTS.md

## Claude Code

`AGENTS.md`, imported above, is the policy every agent follows. The rules below are for Claude Code alone.

### Sessions

* The chat works on the model and effort the owner set.
* The manager is an interactive Claude Code session of Claude Opus 5.5 at high effort, which the chat starts in the tmux session `rach2-manager` on the Mac, in the main checkout, with `CLAUDE_CODE_EFFORT_LEVEL=high` and `CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000`, and briefs to follow the `milestone` skill. It starts that tmux server from its own shell, since macOS's Local Network privacy charges every program in the server to the app that started it, and checks that a Homebrew program there reaches the Lenovo. It checks the workers with `/loop`. The chat and the manager talk with SendMessage: the manager sends each finished step and each incident to the chat, and the chat tells the owner and pushes a big incident to the owner's phone. The Claude app cannot start a session of its own while the owner works over Remote Control, which is why the manager runs in tmux.
* The manager's worker loops, at most three at once, are `tools/flow/worker.sh` with Claude Code, each in a tmux session of its own, with a worktree under `build/worktrees/` and its state under `build/flow/`, as the `milestone` skill says. `tools/flow/worker.sh` runs their sessions at high effort, compacting at 400,000 tokens. They work as the Work section says and start no subagents.
* Fable is the `fable` subagent that `.claude/agents/fable.md` defines: Claude Fable 5.2 at xhigh, which reads files but changes none.
* Rules and facts go into `AGENTS.md`, this file or a skill, the chat's notes on work in progress into `build/flow/coordinator/`, and what a session should remember during a phase into Claude Code's memory files, which `AGENTS.md` says when to fold in and delete.

### Other agents

* Codex, agy and Muse Code run only in the owner's accounts, Codex through the `codex-exec` skill and the other two through the `second-opinion` skill. Each runs one job at a time, and one is unavailable only when it says so.
* When work goes to a worker session, Codex, agy or Muse Code, give it the milestone, the files, the claims the work must make true and what would prove it done, and check every claim in what comes back against the code before using it.
* Never report that a delegated job is running without reading its state in the same turn.

### The chat and the owner

* A message from the owner is answered before any more commands run, even while work goes on.
* Answer each of the owner's questions plainly, as the last thing in the reply, and ask for one decision at a time, in the reply's text: no question boxes, no buttons, and no question in the box at the end of a turn.
* While the owner discusses options, change nothing until the owner says to do it, and follow no other work: the manager sends only the end of its work, a failure or a big incident.
* Tell the owner in the same turn of a delay of a day or more, a cost the owner has not seen, or anything that goes against what the owner asked.
* Pass each failure the manager reports to the owner at once, in the manager's words. Never tell the owner that a run goes on, or that nothing changed, without reading the run's own output in that turn.
* Show research the owner asked for whole, and say which of its findings the plan follows and which it sets aside, and why.
* Before passing on a question or an action of the manager's, check whether the rules or the owner's standing orders already settle it and whether it is the fastest correct path. When they settle it, order that path and tell the owner what was done.
* Push to the owner's phone only what needs the owner now: work stopped on a decision, a dialog or an approval that only the owner can give, led by the problem and what the owner must do. While the owner is awake, also push a Lenovo hang, a failure that stops a milestone and each finished milestone or release. While the owner sleeps, push only what cannot wait for morning, and ask nothing that can. Withdraw a question to the owner that no longer needs them.
* Ask before adding the owner's reflections to `dev-log/reflections.md`.
* At the start of a session and after each compaction, the chat reads the newest `build/flow/coordinator/state-*.md` and checks what it says against the board and the manager. The chat and the manager recover the owner's messages from the session transcripts under `~/.claude/projects/`, and a request not yet done becomes a task; no separate record is kept of each message.
