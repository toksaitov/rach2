@AGENTS.md

## Claude Code

`AGENTS.md`, imported above, is the policy every agent follows. The rules below are for Claude Code alone.

* Work on the model and effort the person running the session set. At most three helpers run at once, each Claude Opus 5.5, the owner's allowance of 2026-10-03, which the owner may withdraw after a phase: no other subagents, agents, workflows, forks or background sessions. A helper works in a git worktree of its own on a branch of its own, starts no subagents, does not push, and leaves its commits for Claude Code to check and merge.
* A helper of the owner's session runs at xhigh effort. In the five accounts the session and its helpers run at high effort and compact at 400,000 tokens; the worker loop sets both with `CLAUDE_CODE_EFFORT_LEVEL=high` and `CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000`, which take precedence over any setting or argument.
* Where Codex is signed in and its plugin for Claude Code is installed, as in the owner's accounts, Codex, one Astra instance at xhigh effort at a time, reviews a phase once it has landed and does other work only when the owner asks; nothing waits for it, and the next unit starts only after the last has answered. Every prompt tells Codex to do the review itself and to start no subagents. Treat Codex as unavailable only when it answers with its usage-limit message. The `codex-units` skill has the mechanics of starting, watching and reading a Codex unit, and names the plugin's script.
* Where they are not, as in the team's accounts on the testing host, there is no Codex review: do the work without one, and leave a review the work needs to the owner.
* When work is handed to a helper or to Codex, give it the phase, the files, the claims the work must make true, and what would prove it done, and check every claim in what comes back against the code before applying it.
* Never report that a delegated job is running without reading its state in the same turn.
