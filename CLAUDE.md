@AGENTS.md

## Claude Code

`AGENTS.md`, imported above, is the policy every agent follows. The rules below are for Claude Code alone.

* Work alone, on the model and effort the owner set for this session: start no subagents of any kind, no agents or tasks, no workflows, no forks and no background sessions.
* Your one helper is the Codex plugin, one Astra instance at xhigh effort at a time, used for review: the next unit starts only after the last has answered. Every prompt tells Codex to do the review itself and to start no subagents. Treat Codex as unavailable only when it answers with its usage-limit message.
* When the owner hands other work to Codex, give it the phase, the files, the claims the work must make true, and what would prove it done, and check every claim in what comes back against the code before applying it.
* Never report that a delegated job is running without reading its state in the same turn.
* The `codex-units` skill has the mechanics of starting, watching and reading a Codex unit.
