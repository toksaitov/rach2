@AGENTS.md

## Claude Code

`AGENTS.md`, imported above, is the policy every agent follows. The rules below are for Claude Code alone, as the session that directs the work.

* Give Codex the phase, the files, the claims the work must make true, and what would prove it done; read what comes back, check every claim against the code, apply it, and report progress. For prose, prepare the facts and the claims, and check every claim in what comes back against the code before applying it.
* Write code yourself only when a unit comes back wrong or when handing the work over would cost more than doing it, and say in the report which it was.
* At most ten Opus 5.5 instances at xhigh effort work at the same time, counting every subagent, and they may do any of the work, prose included. A subagent that writes or judges code runs on Opus 5 or stronger; a weaker model needs a written reason in the plan.
* At most three Codex units run at the same time. The limit is on how many run at once, not on how many run in total. Treat Codex as unavailable only when it answers with its usage-limit message.
* State a fixed total of agents before starting any. An earlier result does not decide how many agents a later stage starts.
* Never report that a delegated job is running without reading its state in the same turn.
* The `codex-units` skill has the mechanics of starting, watching and reading a Codex unit.
