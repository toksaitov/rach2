@AGENTS.md

## Claude Code

`AGENTS.md`, imported above, is the policy every agent follows. The rules below are for Claude Code alone.

* The chat works on the model and effort the owner set.
* The manager is an interactive Claude Code session of Claude Opus 5.5 at high effort, which the chat starts in the tmux session `rach2-manager` on the Mac, in the main checkout, with `CLAUDE_CODE_EFFORT_LEVEL=high` and `CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000`, and briefs to follow the `milestone` skill. It checks the workers with `/loop`. The chat and the manager talk with SendMessage: the manager sends each finished step and each incident to the chat, and the chat tells the owner and pushes a big incident to the owner's phone. The Claude app cannot start a session of its own while the owner works over Remote Control, which is why the manager runs in tmux.
* The manager's worker loops, at most three at once, are `tools/flow/worker.sh` with Claude Code, each in a tmux session of its own, with a worktree under `build/worktrees/` and its state under `build/flow/`, as the `milestone` skill says. Their sessions work as the Work section says and start no subagents. The plan's Fable reviewer is a subagent of the manager's on `claude-fable-5-1`, which reads and changes nothing.
* Worker loop sessions run at high effort and compact at 400,000 tokens; `tools/flow/worker.sh` sets both with `CLAUDE_CODE_EFFORT_LEVEL=high` and `CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000`.
* Codex is signed in only in the owner's accounts. There it reviews plans and milestones and works in the owner's Codex loop. Run it with `codex exec`, as the `codex-exec` skill says, one `gpt-6-astra` job at a time, and treat it as unavailable only when it answers with its usage-limit message.
* When work goes to a worker session or to Codex, give it the milestone, the files, the claims the work must make true and what would prove it done, and check every claim in what comes back against the code before using it.
* Never report that a delegated job is running without reading its state in the same turn.
* While the owner discusses options, change nothing until the owner says to do it.
* Tell the owner in the same turn of a delay of a day or more, a cost the owner has not seen, or anything that goes against what the owner asked.
* Show research the owner asked for whole, and say which of its findings the plan follows and which it sets aside, and why.
* Answer each of the owner's questions plainly, as the last thing in the reply, and ask for one decision at a time, in the reply's text: no question boxes and no buttons.
