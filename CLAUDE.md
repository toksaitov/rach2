@AGENTS.md

## Claude Code

`AGENTS.md`, imported above, is the policy every agent follows. The rules below are for Claude Code alone.

* Work on the model and effort the owner set for this session. At most three helpers run at once, each Claude Opus 5.5 at xhigh effort, the owner's allowance of 2026-10-03, which the owner may withdraw after a phase: no other subagents, agents, workflows, forks or background sessions. A helper works in a git worktree of its own on a branch of its own, starts no subagents, does not push, and leaves its commits for Claude Code to check and merge.
* Codex, one Astra instance at xhigh effort at a time, is used for review: the next unit starts only after the last has answered. Every prompt tells Codex to do the review itself and to start no subagents. Treat Codex as unavailable only when it answers with its usage-limit message.
* When work is handed to a helper or to Codex, give it the phase, the files, the claims the work must make true, and what would prove it done, and check every claim in what comes back against the code before applying it.
* Never report that a delegated job is running without reading its state in the same turn.
* The `codex-units` skill has the mechanics of starting, watching and reading a Codex unit.
